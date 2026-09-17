/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_ota.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506 A/B OTA slot manager (/dev/ota).  See rk3506_ota.h for the
 * protocol contract with the prebuilt U-Boot (CONFIG_ANDROID_AB).
 *
 * Implementation notes:
 *
 *   - All flash access goes through MTD partition devices created with
 *     mtd_partition() from the full-chip SPI NAND MTD.  mtd_partition()
 *     takes its offset/size in geo.blocksize units (2 KiB pages here).
 *     NOTE: the mtd.h doc comment says "offset in bytes"; that is stale -
 *     part_bread/part_bwrite/part_read all add firstblock in block units
 *     (drivers/mtd/mtd_partition.c).
 *
 *   - Metadata read-modify-write: the AvbABData lives at byte offset 2048
 *     of the misc partition, i.e. inside page 1 (2 KiB pages) of erase
 *     block 0.  SPI NAND pages cannot be rewritten in place, so a write
 *     reads back pages 0..1 of erase block 0 (which also preserves the
 *     BCB at offset 0 used by U-Boot's bootloader_message), patches the
 *     metadata bytes, erases erase block 0 and reprograms pages 0..1.
 *
 *   - Slot image access: erase is erase-block granular, program is page
 *     granular.  Userspace streams the new image with write(); the driver
 *     buffers partial pages and programs whole pages.  OTAIOC_UPDATE_COMMIT
 *     pads the final partial page with 0xff.  The readback verify is done
 *     in userspace via read() against the source file.
 *
 *   - Bad blocks: the SPI NAND MTD layer does NOT skip or remap bad
 *     blocks (dhara does that for the /data journal only).  OTAIOC_UPDATE_BEGIN
 *     therefore refuses to start when the target slot window contains a
 *     factory-marked bad block, and any -EIO from erase/program aborts the
 *     update.  A slot is only ever activated after a full readback verify,
 *     so a failed update can never affect the running system.
 *
 *   - Safety: write() to the slot U-Boot would currently boot is refused
 *     (the active slot is the running system).  OTAIOC_SET_ACTIVE refuses
 *     to activate a slot whose first bytes are not the FIT magic unless
 *     RK3506_OTA_FLAG_FORCE is set.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <debug.h>
#include <nuttx/fs/fs.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/mutex.h>

#include "rk3506_ota.h"

#ifdef CONFIG_RK3506_OTA

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Partition layout (nand_firmware/parameter.txt, 512B-sector units) */

#define OTA_MISC_START_SECT      0x5800u
#define OTA_MISC_SECTORS         0x1000u
#define OTA_BOOTA_START_SECT     0x6800u
#define OTA_BOOTA_SECTORS        0x5000u
#define OTA_BOOTB_START_SECT     0xb800u
#define OTA_BOOTB_SECTORS        0x5000u

/* AvbABData (u-boot include/android_avb/avb_ab_flow.h): 32 bytes total,
 * big-endian on media, CRC32 over the first 28 bytes. */

#define AVB_AB_MAGIC             "\0AB0"
#define AVB_AB_MAGIC_LEN         4
#define AVB_AB_MAJOR_VERSION     1
#define AVB_AB_MAX_PRIORITY      15
#define AVB_AB_MAX_TRIES         7

/* Fit magic in the first 4 bytes of the slot (boot_fit checks the FDT
 * header written by mkimage -E) */

#define FIT_MAGIC                0xd00dfeedu

/* Working copy of the metadata in host byte order */

struct avb_ab_data_s
{
  uint8_t  magic[4];
  uint8_t  version_major;
  uint8_t  version_minor;
  uint8_t  reserved1[2];
  uint8_t  slot[2][3];       /* {priority, tries_remaining, successful_boot} */
  uint8_t  slot_reserved[2];
  uint8_t  last_boot;
  uint8_t  reserved2[11];
  uint32_t crc32;            /* big-endian on media */
};

begin_packed_struct struct avb_ab_raw_s
{
  uint8_t data[RK3506_OTA_META_SIZE];
} end_packed_struct;

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_ota_dev_s
{
  mutex_t              lock;        /* Serialises /dev/ota access */
  FAR struct mtd_dev_s *misc;       /* misc partition */
  FAR struct mtd_dev_s *slot[2];    /* boot_a / boot_b partitions */

  uint32_t pagesize;                /* = geo.blocksize (2 KiB) */
  uint32_t erasesize;               /* = geo.erasesize (128 KiB) */
  uint32_t slot_pages;              /* pages per slot partition */

  /* Streaming update state */

  bool     updating;                /* UPDATE_BEGIN done */
  uint8_t  target;                  /* slot selected for write()/read() */
  uint32_t cursor;                  /* bytes programmed so far (page aligned) */
  uint16_t pageoff;                 /* fill level of the page buffer */
  FAR uint8_t *pagebuf;             /* one page staging buffer */
  FAR uint8_t *rdbuf;               /* one page staging buffer for read() */
};

struct rk3506_ota_file_s
{
  bool write_mode;                  /* opened with O_WRONLY/O_RDWR */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ota_open(FAR struct file *filep);
static int ota_close(FAR struct file *filep);
static ssize_t ota_read(FAR struct file *filep, FAR char *buffer,
                        size_t buflen);
static ssize_t ota_write(FAR struct file *filep, FAR const char *buffer,
                         size_t buflen);
static off_t ota_seek(FAR struct file *filep, off_t offset, int whence);
static int ota_ioctl(FAR struct file *filep, int cmd, unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_ota_fops =
{
  ota_open,        /* open   */
  ota_close,       /* close  */
  ota_read,        /* read   */
  ota_write,       /* write  */
  ota_seek,        /* seek   */
  ota_ioctl,       /* ioctl  */
};

static struct rk3506_ota_dev_s g_ota =
{
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ota_crc32
 *
 * Description:
 *   IEEE reflected CRC-32 (poly 0xEDB88320, init and final xor ~0) - the
 *   same algorithm as the u-boot iavb_crc32() used for the AvbABData CRC.
 *
 ****************************************************************************/

static uint32_t ota_crc32(FAR const uint8_t *buf, size_t len)
{
  uint32_t crc = ~0u;

  while (len-- > 0)
    {
      int bit;

      crc ^= *buf++;
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }

  return crc ^ ~0u;
}

/****************************************************************************
 * Name: ota_meta_decode / ota_meta_encode
 *
 * Description:
 *   Marshalling between the on-media big-endian AvbABData and the host
 *   byte order working copy (u-boot avb_ab_data_verify_and_byteswap /
 *   avb_ab_data_update_crc_and_byteswap).  All fields are single bytes
 *   except the trailing crc32, so only the CRC needs byte-order care.
 *
 ****************************************************************************/

static bool ota_meta_decode(FAR const uint8_t *raw,
                            FAR struct avb_ab_data_s *out)
{
  uint32_t stored_crc;
  uint32_t calc_crc;

  if (memcmp(raw, AVB_AB_MAGIC, AVB_AB_MAGIC_LEN) != 0)
    {
      return false;
    }

  memcpy(out, raw, RK3506_OTA_META_SIZE);

  if (out->version_major > AVB_AB_MAJOR_VERSION)
    {
      return false;
    }

  stored_crc = ((uint32_t)raw[28] << 24) | ((uint32_t)raw[29] << 16) |
               ((uint32_t)raw[30] << 8) | (uint32_t)raw[31];
  calc_crc = ota_crc32(raw, RK3506_OTA_META_SIZE - 4);
  return stored_crc == calc_crc;
}

static void ota_meta_encode(FAR const struct avb_ab_data_s *in,
                            FAR uint8_t *raw)
{
  uint32_t crc;

  memcpy(raw, in, RK3506_OTA_META_SIZE);
  crc = ota_crc32(raw, RK3506_OTA_META_SIZE - 4);
  raw[28] = (uint8_t)(crc >> 24);
  raw[29] = (uint8_t)(crc >> 16);
  raw[30] = (uint8_t)(crc >> 8);
  raw[31] = (uint8_t)crc;
}

/****************************************************************************
 * Name: ota_meta_defaults
 *
 * Description:
 *   u-boot avb_ab_data_init(): both slots bootable, A priority 15, B 14,
 *   7 tries each, nothing successful yet.
 *
 ****************************************************************************/

static void ota_meta_defaults(FAR struct avb_ab_data_s *meta)
{
  memset(meta, 0, sizeof(*meta));
  memcpy(meta->magic, AVB_AB_MAGIC, AVB_AB_MAGIC_LEN);
  meta->version_major = AVB_AB_MAJOR_VERSION;
  meta->slot[0][0] = AVB_AB_MAX_PRIORITY;
  meta->slot[0][1] = AVB_AB_MAX_TRIES;
  meta->slot[1][0] = AVB_AB_MAX_PRIORITY - 1;
  meta->slot[1][1] = AVB_AB_MAX_TRIES;
}

/****************************************************************************
 * Name: ota_meta_read / ota_meta_write
 *
 * Description:
 *   Read (and verify) / read-modify-write the AvbABData at misc offset
 *   2048.  The write path preserves everything else in pages 0..1 of
 *   erase block 0 (notably the BCB at offset 0) and erases only erase
 *   block 0 of the misc partition.
 *
 ****************************************************************************/

static int ota_meta_read(FAR struct rk3506_ota_dev_s *priv,
                         FAR struct avb_ab_data_s *meta)
{
  uint8_t raw[RK3506_OTA_META_SIZE];
  ssize_t n;
  off_t page = RK3506_OTA_META_OFF / priv->pagesize;
  off_t inpage = RK3506_OTA_META_OFF % priv->pagesize;
  uint8_t scratch[2048];

  if (priv->pagesize != sizeof(scratch))
    {
      /* Driver is bound to the 2 KiB-page SPI NAND; refuse anything else
       * instead of overflowing the stack buffer. */

      return -EINVAL;
    }

  n = priv->misc->bread(priv->misc, page, 1, scratch);
  if (n < 0)
    {
      return (int)n;
    }

  memcpy(raw, scratch + inpage, RK3506_OTA_META_SIZE);

  if (!ota_meta_decode(raw, meta))
    {
      return -EINVAL;
    }

  return OK;
}

static int ota_meta_write(FAR struct rk3506_ota_dev_s *priv,
                          FAR const struct avb_ab_data_s *meta)
{
  FAR uint8_t *pages;
  off_t firstpage = 0;                    /* erase block 0, pages 0..1 */
  uint32_t npages = (RK3506_OTA_META_OFF + RK3506_OTA_META_SIZE +
                     priv->pagesize - 1) / priv->pagesize;
  ssize_t n;
  int ret;

  pages = kmm_malloc(npages * priv->pagesize);
  if (pages == NULL)
    {
      return -ENOMEM;
    }

  /* Preserve the rest of the pages (BCB at offset 0 lives in page 0) */

  n = priv->misc->bread(priv->misc, firstpage, npages, pages);
  if (n < 0)
    {
      kmm_free(pages);
      return (int)n;
    }

  ota_meta_encode(meta, pages + RK3506_OTA_META_OFF);

  ret = priv->misc->erase(priv->misc, 0, 1);
  if (ret < 0)
    {
      kmm_free(pages);
      return ret;
    }

  n = priv->misc->bwrite(priv->misc, firstpage, npages, pages);
  kmm_free(pages);
  if (n < 0)
    {
      return (int)n;
    }

  return OK;
}

/****************************************************************************
 * Name: ota_pick_active
 *
 * Description:
 *   Same slot choice as u-boot rk_avb_ab_slot_select(): the bootable slot
 *   with the highest priority, ties favouring slot A.  Returns 0/1 or
 *   0xff when no slot is bootable.
 *
 ****************************************************************************/

static uint8_t ota_pick_active(FAR const struct avb_ab_data_s *meta)
{
  bool bootable[2];
  int i;

  for (i = 0; i < 2; i++)
    {
      bootable[i] = meta->slot[i][0] > 0 &&
                    (meta->slot[i][2] != 0 || meta->slot[i][1] > 0);
    }

  if (bootable[0] && bootable[1])
    {
      return meta->slot[1][0] > meta->slot[0][0] ? 1 : 0;
    }

  if (bootable[0])
    {
      return 0;
    }

  if (bootable[1])
    {
      return 1;
    }

  return 0xff;
}

/****************************************************************************
 * Name: ota_slot_fit_ok
 *
 * Description:
 *   Check that the slot partition starts with the FIT magic (i.e. the
 *   external-data FIT image that the pack script puts there).
 *
 ****************************************************************************/

static bool ota_slot_fit_ok(FAR struct rk3506_ota_dev_s *priv, int slot)
{
  uint8_t hdr[4];
  uint32_t magic;
  ssize_t n;

  n = priv->slot[slot]->bread(priv->slot[slot], 0, 1, priv->rdbuf);
  if (n < 0)
    {
      return false;
    }

  memcpy(hdr, priv->rdbuf, 4);
  magic = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
          ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
  return magic == FIT_MAGIC;
}

/****************************************************************************
 * Name: ota_slot_has_badblock
 *
 * Description:
 *   True when any erase block in the slot window is factory-marked bad.
 *   The driver only ever writes a linear image (that is what U-Boot's
 *   boot_fit reads), so bad blocks inside the window cannot be skipped.
 *
 ****************************************************************************/

static bool ota_slot_has_badblock(FAR struct rk3506_ota_dev_s *priv,
                                  int slot)
{
  uint32_t nerases = priv->slot_pages * priv->pagesize / priv->erasesize;
  uint32_t i;

  for (i = 0; i < nerases; i++)
    {
      if (priv->slot[slot]->isbad(priv->slot[slot], (off_t)i) > 0)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: ota_page_program
 *
 * Description:
 *   Program the staging page into the target slot at the cursor.
 *
 ****************************************************************************/

static int ota_page_program(FAR struct rk3506_ota_dev_s *priv)
{
  off_t page = (off_t)(priv->cursor / priv->pagesize);
  ssize_t n;

  n = priv->slot[priv->target]->bwrite(priv->slot[priv->target],
                                       page, 1, priv->pagebuf);
  if (n < 0)
    {
      return (int)n;
    }

  priv->cursor += priv->pagesize;
  priv->pageoff = 0;
  return OK;
}

/****************************************************************************
 * Name: ota_pad_program
 *
 * Description:
 *   Fill the staging page to a full page with 0xff and program it (used
 *   by UPDATE_COMMIT and by the abandon path in close()).
 *
 ****************************************************************************/

static int ota_pad_program(FAR struct rk3506_ota_dev_s *priv)
{
  while (priv->pageoff != 0)
    {
      priv->pagebuf[priv->pageoff++] = 0xff;
      if (priv->pageoff == priv->pagesize)
        {
          return ota_page_program(priv);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: chardev callbacks
 ****************************************************************************/

static int ota_open(FAR struct file *filep)
{
  FAR struct rk3506_ota_file_s *fp;

  fp = kmm_zalloc(sizeof(*fp));
  if (fp == NULL)
    {
      return -ENOMEM;
    }

  fp->write_mode = (filep->f_oflags & (O_WRONLY | O_RDWR)) != 0;
  filep->f_priv = fp;
  return OK;
}

static int ota_close(FAR struct file *filep)
{
  FAR struct rk3506_ota_file_s *fp = filep->f_priv;
  FAR struct rk3506_ota_dev_s *priv = &g_ota;

  if (fp != NULL)
    {
      /* Abandon an unfinished update: pad+commit what was written so the
       * slot is at least page-consistent, then leave it unactivated. */

      nxmutex_lock(&priv->lock);
      if (fp->write_mode && priv->updating)
        {
          ota_pad_program(priv);
          priv->updating = false;
          _warn("WARNING: ota: update of slot %c abandoned before COMMIT\n",
                'a' + priv->target);
        }

      nxmutex_unlock(&priv->lock);
      kmm_free(fp);
      filep->f_priv = NULL;
    }

  return OK;
}

static ssize_t ota_read(FAR struct file *filep, FAR char *buffer,
                        size_t buflen)
{
  FAR struct rk3506_ota_dev_s *priv = &g_ota;
  FAR struct rk3506_ota_file_s *fp = filep->f_priv;
  off_t pos = filep->f_pos;
  size_t done = 0;

  if (fp == NULL)
    {
      return -EBADF;   /* read side must be opened O_RDONLY */
    }

  if (buflen == 0)
    {
      return 0;
    }

  /* Byte-granular read emulated with page reads (round down / carry) */

  while (buflen > 0)
    {
      off_t page = pos / priv->pagesize;
      off_t inpage = pos % priv->pagesize;
      size_t chunk = priv->pagesize - inpage;
      ssize_t n;

      if (chunk > buflen)
        {
          chunk = buflen;
        }

      n = priv->slot[priv->target]->bread(priv->slot[priv->target],
                                          page, 1, priv->rdbuf);
      if (n < 0)
        {
          return done > 0 ? (ssize_t)done : (ssize_t)n;
        }

      memcpy(buffer + done, priv->rdbuf + inpage, chunk);
      done += chunk;
      pos += chunk;
      buflen -= chunk;
    }

  filep->f_pos = pos;
  return (ssize_t)done;
}

static ssize_t ota_write(FAR struct file *filep, FAR const char *buffer,
                         size_t buflen)
{
  FAR struct rk3506_ota_dev_s *priv = &g_ota;
  FAR struct rk3506_ota_file_s *fp = filep->f_priv;
  size_t done = 0;

  if (fp == NULL || !fp->write_mode)
    {
      return -EPERM;
    }

  if (!priv->updating)
    {
      return -EPERM;   /* must call OTAIOC_UPDATE_BEGIN first */
    }

  if ((off_t)priv->cursor + buflen >
      (off_t)priv->slot_pages * priv->pagesize)
    {
      return -ENOSPC;  /* image larger than the slot partition */
    }

  while (buflen > 0)
    {
      size_t chunk = priv->pagesize - priv->pageoff;
      int ret;

      if (chunk > buflen)
        {
          chunk = buflen;
        }

      memcpy(priv->pagebuf + priv->pageoff, buffer + done, chunk);
      priv->pageoff += chunk;
      done += chunk;
      buflen -= chunk;

      if (priv->pageoff == priv->pagesize)
        {
          ret = ota_page_program(priv);
          if (ret < 0)
            {
              return (ssize_t)ret;
            }
        }
    }

  filep->f_pos += done;
  return (ssize_t)done;
}

/****************************************************************************
 * Name: ota_seek
 *
 * Description:
 *   Seek the READ position within the selected slot.  The WRITE path does
 *   not use f_pos at all (it streams from the driver's update cursor set
 *   by OTAIOC_UPDATE_BEGIN), so seeking is always allowed and never
 *   disturbs an in-flight update.
 *
 ****************************************************************************/

static off_t ota_seek(FAR struct file *filep, off_t offset, int whence)
{
  FAR struct rk3506_ota_dev_s *priv = &g_ota;
  off_t maxpos = (off_t)priv->slot_pages * priv->pagesize;
  off_t newpos;

  switch (whence)
    {
      case SEEK_SET:
        newpos = offset;
        break;

      case SEEK_CUR:
        newpos = filep->f_pos + offset;
        break;

      case SEEK_END:
        newpos = maxpos + offset;
        break;

      default:
        return -EINVAL;
    }

  if (newpos < 0)
    {
      return -EINVAL;
    }

  /* == maxpos is allowed (EOF); reads past the end fail in ota_read */

  filep->f_pos = newpos;
  return newpos;
}

static int ota_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct rk3506_ota_dev_s *priv = &g_ota;
  int ret = OK;

  nxmutex_lock(&priv->lock);

  switch (cmd)
    {
      case OTAIOC_STATUS:
        {
          FAR struct ota_status_s *st =
            (FAR struct ota_status_s *)(uintptr_t)arg;
          FAR struct avb_ab_data_s meta;

          memset(st, 0, sizeof(*st));
          st->slot[0].fit_ok = ota_slot_fit_ok(priv, 0);
          st->slot[1].fit_ok = ota_slot_fit_ok(priv, 1);

          ret = ota_meta_read(priv, &meta);
          if (ret == OK)
            {
              int i;

              st->meta_valid = 1;
              st->active_slot = ota_pick_active(&meta);
              st->last_boot = meta.last_boot;
              for (i = 0; i < 2; i++)
                {
                  st->slot[i].priority = meta.slot[i][0];
                  st->slot[i].tries_remaining = meta.slot[i][1];
                  st->slot[i].successful_boot = meta.slot[i][2];
                }
            }
          else
            {
              /* No valid metadata: report it but keep the FIT check */

              st->meta_valid = 0;
              st->active_slot = 0xff;
              ret = OK;
            }
        }
        break;

      case OTAIOC_BOOTCHECK:
        {
          FAR struct ota_bootcheck_s *bc =
            (FAR struct ota_bootcheck_s *)(uintptr_t)arg;
          FAR struct avb_ab_data_s meta;

          memset(bc, 0, sizeof(*bc));

          ret = ota_meta_read(priv, &meta);
          if (ret < 0)
            {
              /* Invalid metadata: rewrite the U-Boot defaults so the
             * counters below start from a defined state. */

              ota_meta_defaults(&meta);
              ret = ota_meta_write(priv, &meta);
              if (ret < 0)
                {
                  break;
                }

              bc->active_slot = 0;
              bc->action = 3;
              bc->tries_remaining = meta.slot[0][1];
              ret = OK;
              break;
            }

          bc->active_slot = ota_pick_active(&meta);

          if (bc->active_slot != 0xff &&
              meta.slot[bc->active_slot][2] == 0)
            {
              /* Not yet confirmed: burn one try.  When the last try is
               * spent, make the slot unbootable and hand the boot back to
               * the other slot (rollback at the NEXT reboot).  U-Boot's
               * boot_fit path does not decrement tries itself, so this
               * userspace hook from rcS is the retry/rollback mechanism. */

              uint8_t other = bc->active_slot == 0 ? 1 : 0;

              if (meta.slot[bc->active_slot][1] > 0)
                {
                  meta.slot[bc->active_slot][1]--;
                  bc->action = 1;
                }

              bc->tries_remaining = meta.slot[bc->active_slot][1];
              if (bc->tries_remaining == 0)
                {
                  meta.slot[bc->active_slot][0] = 0;
                  meta.slot[bc->active_slot][1] = 0;
                  meta.slot[bc->active_slot][2] = 0;
                  meta.slot[other][0] = AVB_AB_MAX_PRIORITY;
                  meta.slot[other][1] = AVB_AB_MAX_TRIES;
                  bc->action = 2;
                }

              ret = ota_meta_write(priv, &meta);
            }
          else if (bc->active_slot != 0xff)
            {
              bc->tries_remaining = meta.slot[bc->active_slot][1];
            }
        }
        break;

      case OTAIOC_CONFIRM:
        {
          FAR int *out = (FAR int *)(uintptr_t)arg;
          FAR struct avb_ab_data_s meta;
          uint8_t active;

          ret = ota_meta_read(priv, &meta);
          if (ret < 0)
            {
              break;
            }

          active = ota_pick_active(&meta);
          if (active == 0xff)
            {
              ret = -ENOENT;
              break;
            }

          meta.slot[active][2] = 1;   /* successful_boot */
          meta.last_boot = active;
          ret = ota_meta_write(priv, &meta);
          if (ret == OK && out != NULL)
            {
              *out = (int)active;
            }
        }
        break;

      case OTAIOC_SET_ACTIVE:
      case OTAIOC_SELECT:
        {
          int slotarg = (int)arg & ~RK3506_OTA_FLAG_FORCE;
          bool force = (arg & RK3506_OTA_FLAG_FORCE) != 0;
          FAR struct avb_ab_data_s meta;

          if (slotarg != 0 && slotarg != 1)
            {
              ret = -EINVAL;
              break;
            }

          if (cmd == OTAIOC_SELECT)
            {
              priv->target = (uint8_t)slotarg;   /* read-side selection */
              break;
            }

          if (!force && !ota_slot_fit_ok(priv, slotarg))
            {
              _err("ERROR: ota: slot %c has no FIT image; refusing to "
                   "activate\n", 'a' + slotarg);
              ret = -EINVAL;
              break;
            }

          ret = ota_meta_read(priv, &meta);
          if (ret < 0)
            {
              ota_meta_defaults(&meta);
            }

          /* Android bootctl setActiveBootSlot: target gets max priority
           * with fresh tries; make sure the other slot cannot out-rank
           * it. */

          meta.slot[slotarg][0] = AVB_AB_MAX_PRIORITY;
          meta.slot[slotarg][1] = AVB_AB_MAX_TRIES;
          meta.slot[slotarg][2] = 0;
          if (meta.slot[slotarg ^ 1][0] >= AVB_AB_MAX_PRIORITY)
            {
              meta.slot[slotarg ^ 1][0] = AVB_AB_MAX_PRIORITY - 1;
            }

          ret = ota_meta_write(priv, &meta);
        }
        break;

      case OTAIOC_UPDATE_BEGIN:
        {
          int slotarg = (int)arg & ~RK3506_OTA_FLAG_FORCE;
          bool force = (arg & RK3506_OTA_FLAG_FORCE) != 0;
          FAR struct avb_ab_data_s meta;
          uint8_t active;
          uint32_t nerases;
          uint32_t i;

          if (priv->updating)
            {
              ret = -EBUSY;
              break;
            }

          if (slotarg != 0 && slotarg != 1)
            {
              ret = -EINVAL;
              break;
            }

          if (ota_slot_has_badblock(priv, slotarg))
            {
              _err("ERROR: ota: slot %c window contains a bad block; "
                   "linear update not possible\n", 'a' + slotarg);
              ret = -EIO;
              break;
            }

          ret = ota_meta_read(priv, &meta);
          active = ret < 0 ? 0 : ota_pick_active(&meta);

          if (active == 0xff)
            {
              active = 0;   /* nothing bootable: protect slot A by default */
            }

          if (!force && slotarg == (int)active)
            {
              _err("ERROR: ota: slot %c is the active slot; refusing to "
                   "erase\n", 'a' + slotarg);
              ret = -EBUSY;
              break;
            }

          /* Erase the whole slot partition */

          nerases = priv->slot_pages * priv->pagesize / priv->erasesize;
          for (i = 0; i < nerases; i++)
            {
              ret = priv->slot[slotarg]->erase(priv->slot[slotarg],
                                               (off_t)i, 1);
              if (ret < 0)
                {
                  break;
                }
            }

          if (ret < 0)
            {
              break;
            }

          priv->updating = true;
          priv->target   = (uint8_t)slotarg;
          priv->cursor   = 0;
          priv->pageoff  = 0;
          ret = OK;
        }
        break;

      case OTAIOC_UPDATE_COMMIT:
        {
          FAR ssize_t *written = (FAR ssize_t *)(uintptr_t)arg;

          if (!priv->updating)
            {
              ret = -EPERM;
              break;
            }

          ret = ota_pad_program(priv);
          if (ret == OK && written != NULL)
            {
              *written = (ssize_t)priv->cursor;
            }

          priv->updating = false;
        }
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_ota_initialize
 *
 * Description:
 *   Create the misc / boot_a / boot_b MTD partitions and register
 *   /dev/ota.  Called from board bringup after the SPI NAND MTD exists.
 *
 ****************************************************************************/

int rk3506_ota_initialize(FAR struct mtd_dev_s *mtd)
{
  struct mtd_geometry_s geo;
  FAR struct rk3506_ota_dev_s *priv = &g_ota;
  off_t misc_start;
  off_t boota_start;
  off_t bootb_start;
  int ret;

  ret = mtd->ioctl(mtd, MTDIOC_GEOMETRY, (unsigned long)(uintptr_t)&geo);
  if (ret < 0)
    {
      _err("ERROR: ota: geometry ioctl failed: %d\n", ret);
      return ret;
    }

  priv->pagesize  = geo.blocksize;   /* 2 KiB */
  priv->erasesize = geo.erasesize;   /* 128 KiB */
  priv->slot_pages = (uint32_t)OTA_BOOTA_SECTORS * 512 / priv->pagesize;

  /* mtd_partition() offsets are in geo.blocksize units (2 KiB pages);
   * see the note in the file header. */

  misc_start  = (off_t)OTA_MISC_START_SECT * 512 / priv->pagesize;
  boota_start = (off_t)OTA_BOOTA_START_SECT * 512 / priv->pagesize;
  bootb_start = (off_t)OTA_BOOTB_START_SECT * 512 / priv->pagesize;

  priv->misc = mtd_partition(mtd, misc_start,
                             (off_t)OTA_MISC_SECTORS * 512 / priv->pagesize);
  priv->slot[0] = mtd_partition(mtd, boota_start,
                                (off_t)OTA_BOOTA_SECTORS * 512 /
                                priv->pagesize);
  priv->slot[1] = mtd_partition(mtd, bootb_start,
                                (off_t)OTA_BOOTB_SECTORS * 512 /
                                priv->pagesize);

  if (priv->misc == NULL || priv->slot[0] == NULL || priv->slot[1] == NULL)
    {
      _err("ERROR: ota: mtd_partition failed\n");
      return -ENODEV;
    }

  priv->pagebuf = kmm_malloc(priv->pagesize);
  priv->rdbuf = kmm_malloc(priv->pagesize);
  if (priv->pagebuf == NULL || priv->rdbuf == NULL)
    {
      kmm_free(priv->pagebuf);
      kmm_free(priv->rdbuf);
      priv->pagebuf = NULL;
      priv->rdbuf = NULL;
      _err("ERROR: ota: page buffer alloc failed\n");
      return -ENOMEM;
    }

  ret = register_driver(RK3506_OTA_DEVPATH, &g_ota_fops, 0666, priv);
  if (ret < 0)
    {
      kmm_free(priv->pagebuf);
      kmm_free(priv->rdbuf);
      priv->pagebuf = NULL;
      priv->rdbuf = NULL;
      _err("ERROR: ota: register_driver failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "OTA: /dev/ota ready (misc@%luK boot_a@%luK "
         "boot_b@%luK, %lu x %luK blocks)\n",
         (unsigned long)(OTA_MISC_START_SECT * 512 / 1024),
         (unsigned long)(OTA_BOOTA_START_SECT * 512 / 1024),
         (unsigned long)(OTA_BOOTB_START_SECT * 512 / 1024),
         (unsigned long)(priv->slot_pages * priv->pagesize / priv->erasesize),
         (unsigned long)(priv->erasesize / 1024));
  return OK;
}

#endif /* CONFIG_RK3506_OTA */
