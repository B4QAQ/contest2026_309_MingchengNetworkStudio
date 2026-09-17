/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_spinand_fspi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI NAND MTD driver for the HD-RK3506-EVM, running on the RK3506
 * FSPI (SFC) controller via the public rk3506_fspi_nand_op()
 * command primitive (1-1-1 mode, PIO).
 *
 * The command/state-machine flow is ported from the Linux SPI NAND
 * core (SDK kernel-6.1/drivers/mtd/nand/spi/core.c) and the chip
 * geometry from the Xincun table (kernel-6.1/.../spi/xincun.c,
 * XCSP2AAPK / XCSP1AAPK).  The on-die ECC engine is enabled via the
 * Feature-Configuration register (bit 4) exactly as
 * spinand_ecc_enable() does; read ECC results are decoded from the
 * GET FEATURE status register bits [5:4] per xincun's
 * xcsp2aapk_ecc_get_status().
 *
 * MTD geometry contract (consumed by dhara, drivers/mtd/dhara.c):
 *   blocksize   = page size          (2048 B)
 *   erasesize   = pages/block * page (128 KiB)
 *   neraseblocks                     (2048 / 1024)
 *   bread/bwrite are page granular; berase is erase-block granular.
 *   bread returns -EUCLEAN for ECC-corrected pages (dhara tolerates)
 *   and -EBADMSG for uncorrectable ones.
 *
 * Bad-block management: the factory BBM lives in the first two OOB
 * bytes of the first two pages of each block.  Blocks are checked
 * lazily from the isbad() MTD method (one 2-byte raw OOB read per
 * page) and the result is cached; markbad() programs a 0x00 marker.
 * Dhara additionally remaps bad blocks at the journal level.
 *
 * Partitioning note: the chip is shared with the Rockchip boot
 * chain (u-boot/FIT partitions per nand_firmware/parameter.txt).
 * bringup must only hand dhara an mtd_partition() slice covering the
 * "userdata" region - NEVER the raw whole-chip MTD.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <debug.h>
#include <syslog.h>

#include <nuttx/kmalloc.h>
#include <nuttx/mtd/mtd.h>

#include "rk3506_fspi.h"

/* All transfers use CS0 */

#define SPINAND_CS_UNUSED       0

/* SPI NAND opcodes (Linux include/linux/mtd/spinand.h) */

#define SPINAND_RESET           0xff
#define SPINAND_WREN            0x06
#define SPINAND_GET_FEATURE     0x0f
#define SPINAND_SET_FEATURE     0x1f
#define SPINAND_READ_PAGE       0x13    /* Array-to-cache load */
#define SPINAND_PROG_LOAD       0x02
#define SPINAND_PROG_EXEC       0x10
#define SPINAND_BLK_ERASE       0xd8
#define SPINAND_READ_CACHE_1B   0x0b    /* Fast read from cache, 1 dummy */

/* Feature register addresses */

#define SPINAND_REG_CFG         0xb0
#define SPINAND_REG_STATUS      0xc0

/* Status register bits */

#define SPINAND_STATUS_BUSY              (1u << 0)
#define SPINAND_STATUS_ERASE_FAILED      (1u << 2)
#define SPINAND_STATUS_PROG_FAILED       (1u << 3)
#define SPINAND_STATUS_ECC_NO_BITFLIPS   (0x0u << 4)
#define SPINAND_STATUS_ECC_HAS_BITFLIPS  (0x1u << 4)
#define SPINAND_STATUS_ECC_UNCOR_ERROR   (0x2u << 4)
#define XINCUN_STATUS_ECC_HAS_BITFLIPS_T (0x3u << 4)
#define SPINAND_STATUS_ECC_MASK          (0x3u << 4)

/* Configuration register bits */

#define SPINAND_CFG_ECC_ENABLE  (1u << 4)

/* Wait timings (Linux spinand.h) */

#define SPINAND_RESET_INIT_US   5
#define SPINAND_RESET_POLL_US   5
#define SPINAND_READ_INIT_US    6
#define SPINAND_READ_POLL_US    5
#define SPINAND_WRITE_INIT_US   75
#define SPINAND_WRITE_POLL_US   15
#define SPINAND_ERASE_INIT_US   250
#define SPINAND_ERASE_POLL_US   50
#define SPINAND_WAIT_TIMEOUT_US (400u * 1000u)

/* Bad-block states (2 bits per block, lazy evaluation) */

#define BB_UNKNOWN              0u
#define BB_GOOD                 1u
#define BB_BAD                  2u

/* Raw OOB read: 2 BBM bytes at the start of the OOB area */

#define SPINAND_BBM_LEN         2

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Chip geometry table entry (from Linux xincun.c table) */

struct spinand_id_s
{
  uint8_t  mfr;         /* Manufacturer ID (id[0]) */
  uint8_t  memorg;      /* Memory-organization ID (id[1]) */
  const char *name;
  uint16_t pagesize;    /* Main-area page size, bytes */
  uint16_t oobsize;     /* OOB size per page, bytes */
  uint16_t ppb;         /* Pages per erase block */
  uint32_t nblocks;     /* Erase blocks total */
};

/* SPI NAND device private state */

struct rk3506_spinand_s
{
  struct mtd_dev_s mtd;         /* MTD interface (must be first) */

  const struct spinand_id_s *id;
  uint32_t pagesize;            /* 2048 */
  uint32_t oobsize;             /* 128 */
  uint32_t ppb;                 /* 64 */
  uint32_t nblocks;             /* 2048 */
  uint8_t  *badstate;           /* Per-block lazy bad-block cache */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Chip table: only parts with an authoritative Linux SDK table entry
 * are listed (AGENTS 3.4.5: no guessed geometries).
 */

static const struct spinand_id_s g_spinand_ids[] =
{
  {
    .mfr      = 0x8c,
    .memorg   = 0xa1,
    .name     = "XCSP2AAPK",
    .pagesize = 2048,
    .oobsize  = 128,
    .ppb      = 64,
    .nblocks  = 2048
  },
  {
    .mfr      = 0x8c,
    .memorg   = 0x01,
    .name     = "XCSP1AAPK",
    .pagesize = 2048,
    .oobsize  = 128,
    .ppb      = 64,
    .nblocks  = 1024
  },
};

#define NSPINAND_IDS (sizeof(g_spinand_ids) / sizeof(g_spinand_ids[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: spinand_wait_ready
 *
 * Description:
 *   Poll the GET FEATURE status register until the busy bit clears.
 *   Mirrors Linux spinand_wait(): initial delay, then poll interval,
 *   bounded by SPINAND_WAIT_TIMEOUT_US.
 *
 * Input Parameters:
 *   init_us - Initial sleep before the first poll.
 *   poll_us - Delay between polls.
 *   status  - If non-NULL, receives the last status value.
 *
 * Returned Value:
 *   Zero when the device is ready; -ETIMEDOUT when not ready within
 *   the timeout.
 *
 ****************************************************************************/

static int spinand_wait_ready(uint32_t init_us, uint32_t poll_us,
                              uint8_t *status)
{
  uint32_t elapsed_us = 0;
  uint8_t  s = 0;
  int ret;

  up_udelay(init_us);
  elapsed_us += init_us;

  for (; ; )
    {
      ret = rk3506_fspi_nand_op(SPINAND_GET_FEATURE, 1,
                                SPINAND_REG_STATUS, 0, false, 1, &s);
      if (ret < 0)
        {
          return ret;
        }

      if (!(s & SPINAND_STATUS_BUSY))
        {
          break;
        }

      if (elapsed_us >= SPINAND_WAIT_TIMEOUT_US)
        {
          ferr("ERROR: SPI NAND wait timeout (status 0x%02x)\n", s);
          return -ETIMEDOUT;
        }

      up_udelay(poll_us);
      elapsed_us += poll_us;
    }

  if (status != NULL)
    {
      *status = s;
    }

  return OK;
}

/****************************************************************************
 * Name: spinand_get_cfg / spinand_ecc_enable
 *
 * Description:
 *   Read the CFG feature register / set or clear the on-die ECC
 *   engine enable bit (Linux spinand_ecc_enable()).
 *
 ****************************************************************************/

static int spinand_get_cfg(uint8_t *cfg)
{
  return rk3506_fspi_nand_op(SPINAND_GET_FEATURE, 1, SPINAND_REG_CFG,
                             0, false, 1, cfg);
}

static int spinand_ecc_enable(bool enable)
{
  uint8_t cfg;
  int ret;

  ret = spinand_get_cfg(&cfg);
  if (ret < 0)
    {
      return ret;
    }

  if (enable)
    {
      cfg |= SPINAND_CFG_ECC_ENABLE;
    }
  else
    {
      cfg &= (uint8_t)~SPINAND_CFG_ECC_ENABLE;
    }

  return rk3506_fspi_nand_op(SPINAND_SET_FEATURE, 1, SPINAND_REG_CFG,
                             0, true, 1, &cfg);
}

/****************************************************************************
 * Name: spinand_decode_ecc_status
 *
 * Description:
 *   Decode the GET FEATURE status of a completed page load into an
 *   MTD-style result.  Per xincun.c xcsp2aapk_ecc_get_status().
 *
 * Returned Value:
 *   0        - no bitflips
 *   1        - corrected bitflips (report -EUCLEAN up the stack)
 *   -EBADMSG - uncorrectable
 *
 ****************************************************************************/

static int spinand_decode_ecc_status(uint8_t status)
{
  switch (status & SPINAND_STATUS_ECC_MASK)
    {
    case SPINAND_STATUS_ECC_NO_BITFLIPS:
      return 0;

    case SPINAND_STATUS_ECC_HAS_BITFLIPS:
      return 1;

    case SPINAND_STATUS_ECC_UNCOR_ERROR:
      return -EBADMSG;

    case XINCUN_STATUS_ECC_HAS_BITFLIPS_T:
      /* Xincun: threshold crossing still means corrected (strength) */

      return 1;

    default:
      return -EBADMSG;
    }
}

/****************************************************************************
 * Name: spinand_load_page
 *
 * Description:
 *   Load one page from the array into the cache (0x13) and wait for
 *   tR.  Returns the last status in *status (may be NULL).
 *
 ****************************************************************************/

static int spinand_load_page(uint32_t row, FAR uint8_t *status)
{
  int ret;

  ret = rk3506_fspi_nand_op(SPINAND_READ_PAGE, 3, row, 0, false, 0, NULL);
  if (ret < 0)
    {
      return ret;
    }

  return spinand_wait_ready(SPINAND_READ_INIT_US, SPINAND_READ_POLL_US,
                            status);
}

/****************************************************************************
 * Name: spinand_read_page
 *
 * Description:
 *   Read one whole main-area page with the on-die ECC engine.
 *
 * Returned Value:
 *   0 on clean read; -EUCLEAN when the ECC engine corrected bitflips;
 *   -EBADMSG for uncorrectable pages; other negative errno on bus
 *   failures.
 *
 ****************************************************************************/

static int spinand_read_page(FAR struct rk3506_spinand_s *priv,
                             uint32_t page, FAR uint8_t *buf)
{
  uint8_t status = 0;
  int ret;

  ret = spinand_load_page(page, &status);
  if (ret < 0)
    {
      return ret;
    }

  /* Cache -> host (full main area) */

  ret = rk3506_fspi_nand_op(SPINAND_READ_CACHE_1B, 2, 0, 1, false,
                            priv->pagesize, buf);
  if (ret < 0)
    {
      return ret;
    }

  /* Decode the on-die ECC result of the load */

  ret = spinand_decode_ecc_status(status);
  if (ret < 0)
    {
      ferr("ERROR: page %lu uncorrectable ECC (status 0x%02x)\n",
           (unsigned long)page, status);
      return ret;
    }

  return ret > 0 ? -EUCLEAN : OK;
}

/****************************************************************************
 * Name: spinand_write_page
 *
 * Description:
 *   Program one full page: WREN, PROG LOAD (main area, column 0; the
 *   page is freshly erased so the untouched cache tail is 0xff),
 *   PROG EXEC, wait, check the program-failed status.
 *
 ****************************************************************************/

static int spinand_write_page(FAR struct rk3506_spinand_s *priv,
                              uint32_t page, FAR const uint8_t *buf)
{
  uint8_t status = 0;
  int ret;

  ret = rk3506_fspi_nand_op(SPINAND_WREN, 0, 0, 0, false, 0, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3506_fspi_nand_op(SPINAND_PROG_LOAD, 2, 0, 0, true,
                            priv->pagesize, (FAR uint8_t *)buf);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3506_fspi_nand_op(SPINAND_PROG_EXEC, 3, page, 0, false, 0,
                            NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = spinand_wait_ready(SPINAND_WRITE_INIT_US, SPINAND_WRITE_POLL_US,
                           &status);
  if (ret < 0)
    {
      return ret;
    }

  if (status & SPINAND_STATUS_PROG_FAILED)
    {
      ferr("ERROR: page %lu program failed (status 0x%02x)\n",
           (unsigned long)page, status);
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: spinand_erase_block
 *
 * Description:
 *   Erase one erase block: WREN, BLOCK ERASE (row = first page of the
 *   block), wait, check ERASE_FAILED.
 *
 ****************************************************************************/

static int spinand_erase_block(FAR struct rk3506_spinand_s *priv,
                               uint32_t block)
{
  uint32_t row = block * priv->ppb;
  uint8_t status = 0;
  int ret;

  ret = rk3506_fspi_nand_op(SPINAND_WREN, 0, 0, 0, false, 0, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3506_fspi_nand_op(SPINAND_BLK_ERASE, 3, row, 0, false, 0, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = spinand_wait_ready(SPINAND_ERASE_INIT_US, SPINAND_ERASE_POLL_US,
                           &status);
  if (ret < 0)
    {
      return ret;
    }

  if (status & SPINAND_STATUS_ERASE_FAILED)
    {
      ferr("ERROR: block %lu erase failed (status 0x%02x)\n",
           (unsigned long)block, status);
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: spinand_read_oob_raw
 *
 * Description:
 *   Read raw (ECC-disabled) OOB bytes of one page.  Used for BBM
 *   detection: with the ECC engine disabled the cache read returns
 *   the OOB area verbatim at column >= pagesize.
 *
 ****************************************************************************/

static int spinand_read_oob_raw(FAR struct rk3506_spinand_s *priv,
                                uint32_t page, uint16_t oob_offset,
                                uint16_t len, FAR uint8_t *buf)
{
  uint8_t cfg = 0;
  bool ecc_was_on;
  int ret;

  /* Disable the on-die ECC engine for a raw read */

  ret = spinand_get_cfg(&cfg);
  if (ret < 0)
    {
      return ret;
    }

  ecc_was_on = (cfg & SPINAND_CFG_ECC_ENABLE) != 0;

  if (ecc_was_on)
    {
      ret = spinand_ecc_enable(false);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* Array -> cache, then read the requested OOB slice */

  ret = spinand_load_page(page, NULL);
  if (ret >= 0)
    {
      ret = rk3506_fspi_nand_op(SPINAND_READ_CACHE_1B, 2,
                                priv->pagesize + oob_offset, 1, false,
                                len, buf);
      if (ret >= 0)
        {
          ret = OK;
        }
    }

  if (ecc_was_on)
    {
      int r2 = spinand_ecc_enable(true);
      if (r2 < 0)
        {
          return r2;
        }
    }

  return ret;
}

/****************************************************************************
 * Name: spinand_scan_block_bad
 *
 * Description:
 *   Evaluate the factory bad-block markers of one block: a block is
 *   bad when the first BBM byte of page 0 or page 1 is not 0xff.
 *
 ****************************************************************************/

static bool spinand_scan_block_bad(FAR struct rk3506_spinand_s *priv,
                                   uint32_t block)
{
  uint32_t base = block * priv->ppb;
  uint8_t  bbm[SPINAND_BBM_LEN];
  int ret;
  int p;

  for (p = 0; p < 2; p++)
    {
      memset(bbm, 0xff, sizeof(bbm));
      ret = spinand_read_oob_raw(priv, base + p, 0, SPINAND_BBM_LEN, bbm);
      if (ret < 0)
        {
          /* Unreadable marker: treat as bad, the safer direction */

          ferr("ERROR: BBM read failed on block %lu page %d: %d\n",
               (unsigned long)block, p, ret);
          return true;
        }

      if (bbm[0] != 0xff)
        {
          _info("SPI NAND: block %lu marked bad (BBM %02x %02x)\n",
                (unsigned long)block, bbm[0], bbm[1]);
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: spinand_isbad / spinand_markbad
 *
 * Description:
 *   MTD bad-block methods (lazy scan + cache).
 *
 ****************************************************************************/

static int spinand_isbad(FAR struct mtd_dev_s *dev, off_t block)
{
  FAR struct rk3506_spinand_s *priv = (FAR struct rk3506_spinand_s *)dev;
  uint8_t state;

  if (block < 0 || (uint32_t)block >= priv->nblocks)
    {
      return -EINVAL;
    }

  state = priv->badstate[block];
  if (state == BB_UNKNOWN)
    {
      state = spinand_scan_block_bad(priv, block) ? BB_BAD : BB_GOOD;
      priv->badstate[block] = state;
    }

  return state == BB_BAD;
}

static int spinand_markbad(FAR struct mtd_dev_s *dev, off_t block)
{
  FAR struct rk3506_spinand_s *priv = (FAR struct rk3506_spinand_s *)dev;
  uint8_t zero[SPINAND_BBM_LEN] = { 0x00, 0x00 };
  uint8_t status = 0;
  uint32_t page;
  bool ecc_was_on;
  uint8_t cfg = 0;
  int ret;

  if (block < 0 || (uint32_t)block >= priv->nblocks)
    {
      return -EINVAL;
    }

  page = (uint32_t)block * priv->ppb;

  /* The marker must be programmed raw (ECC off) */

  ret = spinand_get_cfg(&cfg);
  if (ret < 0)
    {
      return ret;
    }

  ecc_was_on = (cfg & SPINAND_CFG_ECC_ENABLE) != 0;
  if (ecc_was_on)
    {
      ret = spinand_ecc_enable(false);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = rk3506_fspi_nand_op(SPINAND_WREN, 0, 0, 0, false, 0, NULL);
  if (ret >= 0)
    {
      /* PROG LOAD with column = OOB start: only the marker bytes are
       * written (page 0 of a block being marked is not expected to
       * hold valid data).
       */

      ret = rk3506_fspi_nand_op(SPINAND_PROG_LOAD, 2, priv->pagesize,
                                0, true, sizeof(zero), zero);
      if (ret >= 0)
        {
          ret = rk3506_fspi_nand_op(SPINAND_PROG_EXEC, 3, page,
                                    0, false, 0, NULL);
          if (ret >= 0)
            {
              ret = spinand_wait_ready(SPINAND_WRITE_INIT_US,
                                       SPINAND_WRITE_POLL_US, &status);
              if (ret >= 0 &&
                  (status & SPINAND_STATUS_PROG_FAILED))
                {
                  ret = -EIO;
                }
            }
        }
    }

  if (ecc_was_on)
    {
      int r2 = spinand_ecc_enable(true);
      if (r2 < 0)
        {
          return r2;
        }
    }

  if (ret >= 0)
    {
      priv->badstate[block] = BB_BAD;
      _info("SPI NAND: block %lu marked bad\n", (unsigned long)block);
      ret = OK;
    }

  return ret;
}

/****************************************************************************
 * Name: spinand_erase / spinand_bread / spinand_bwrite / spinand_ioctl
 *
 * Description:
 *   MTD interface methods.
 *
 ****************************************************************************/

static int spinand_erase(FAR struct mtd_dev_s *dev, off_t startblock,
                         size_t nblocks)
{
  FAR struct rk3506_spinand_s *priv = (FAR struct rk3506_spinand_s *)dev;
  size_t i;
  int ret;

  finfo("startblock: %08lx nblocks: %d\n", (long)startblock,
        (int)nblocks);

  for (i = 0; i < nblocks; i++)
    {
      uint32_t block = startblock + i;

      if (block >= priv->nblocks)
        {
          ferr("ERROR: erase block %lu out of range\n",
               (unsigned long)block);
          return -EINVAL;
        }

      ret = spinand_erase_block(priv, block);
      if (ret < 0)
        {
          return ret;
        }
    }

  return (int)nblocks;
}

static ssize_t spinand_bread(FAR struct mtd_dev_s *dev, off_t startpage,
                             size_t npages, FAR uint8_t *buffer)
{
  FAR struct rk3506_spinand_s *priv = (FAR struct rk3506_spinand_s *)dev;
  uint32_t npages_total = priv->nblocks * priv->ppb;
  bool corrected = false;
  size_t i;
  int ret;

  finfo("startpage: %08lx npages: %d\n", (long)startpage, (int)npages);

  for (i = 0; i < npages; i++)
    {
      uint32_t page = startpage + i;

      if (page >= npages_total)
        {
          ferr("ERROR: read page %lu out of range\n",
               (unsigned long)page);
          return -EINVAL;
        }

      ret = spinand_read_page(priv, page, buffer + i * priv->pagesize);
      if (ret == -EUCLEAN)
        {
          corrected = true;
        }
      else if (ret < 0)
        {
          return ret;
        }
    }

  /* Report correctable ECC so dhara can schedule a refresh; dhara
   * treats -EUCLEAN as success (drivers/mtd/dhara.c dhara_nand_read).
   */

  if (corrected)
    {
      return -EUCLEAN;
    }

  return (ssize_t)npages;
}

static ssize_t spinand_bwrite(FAR struct mtd_dev_s *dev, off_t startpage,
                              size_t npages, FAR const uint8_t *buffer)
{
  FAR struct rk3506_spinand_s *priv = (FAR struct rk3506_spinand_s *)dev;
  uint32_t npages_total = priv->nblocks * priv->ppb;
  size_t i;
  int ret;

  finfo("startpage: %08lx npages: %d\n", (long)startpage, (int)npages);

  for (i = 0; i < npages; i++)
    {
      uint32_t page = startpage + i;

      if (page >= npages_total)
        {
          ferr("ERROR: write page %lu out of range\n",
               (unsigned long)page);
          return -EINVAL;
        }

      ret = spinand_write_page(priv, page,
                               buffer + i * priv->pagesize);
      if (ret < 0)
        {
          return ret;
        }
    }

  return (ssize_t)npages;
}

static int spinand_ioctl(FAR struct mtd_dev_s *dev, int cmd,
                         unsigned long arg)
{
  FAR struct rk3506_spinand_s *priv = (FAR struct rk3506_spinand_s *)dev;
  int ret = -EINVAL;

  finfo("cmd: %d\n", cmd);

  switch (cmd)
    {
    case MTDIOC_GEOMETRY:
      {
        FAR struct mtd_geometry_s *geo =
          (FAR struct mtd_geometry_s *)((uintptr_t)arg);

        if (geo != NULL)
          {
            geo->blocksize    = priv->pagesize;
            geo->erasesize    = priv->pagesize * priv->ppb;
            geo->neraseblocks = priv->nblocks;
            ret               = OK;
          }
      }
      break;

    case MTDIOC_ERASESTATE:
      {
        FAR uint8_t *erasestate = (FAR uint8_t *)((uintptr_t)arg);

        *erasestate = 0xff;
        ret = OK;
      }
      break;

    default:
      ret = -ENOTTY;
      break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_spinand_fspi_initialize
 *
 * Description:
 *   Probe the SPI NAND on FSPI CS0 and return an MTD device for the
 *   WHOLE chip.  Callers must wrap it with mtd_partition() before
 *   handing it to dhara_initialize() - the raw device overlaps the
 *   u-boot/FIT partitions written by the Rockchip upgrade tool.
 *
 *   The FSPI clock and pin mux must already be configured
 *   (rk3506_fspi_clock_init + rk3506_ioc_fspi_setup, as done in the
 *   board bring-up before calling this function); controller reset
 *   and DLL tuning are (re)performed here via rk3506_fspi_probe().
 *
 * Returned Value:
 *   The whole-chip MTD device, or NULL on probe failure.
 *
 ****************************************************************************/

FAR struct mtd_dev_s *rk3506_spinand_fspi_initialize(void)
{
  FAR struct rk3506_spinand_s *priv = NULL;
  uint8_t id[4];
  int ret;

  /* Controller bring-up + DLL tuning + READ_ID */

  ret = rk3506_fspi_probe(id);
  if (ret < 0)
    {
      ferr("ERROR: FSPI probe failed: %d\n", ret);
      return NULL;
    }

  syslog(LOG_INFO, "FSPI: SPI NAND READ_ID = %02x %02x %02x %02x\n",
         id[0], id[1], id[2], id[3]);

  /* Match the ID against the chip table */

  for (size_t i = 0; i < NSPINAND_IDS; i++)
    {
      if (id[0] == g_spinand_ids[i].mfr &&
          id[1] == g_spinand_ids[i].memorg)
        {
          priv = kmm_zalloc(sizeof(*priv));
          if (priv == NULL)
            {
              return NULL;
            }

          priv->id = &g_spinand_ids[i];
          break;
        }
    }

  if (priv == NULL)
    {
      ferr("ERROR: unknown SPI NAND (mfr 0x%02x memorg 0x%02x)\n",
           id[0], id[1]);
      return NULL;
    }

  priv->pagesize = priv->id->pagesize;
  priv->oobsize  = priv->id->oobsize;
  priv->ppb      = priv->id->ppb;
  priv->nblocks  = priv->id->nblocks;

  /* Bad-block state cache (BB_UNKNOWN == 0 after zalloc) */

  priv->badstate = kmm_zalloc(priv->nblocks);
  if (priv->badstate == NULL)
    {
      kmm_free(priv);
      return NULL;
    }

  /* Reset the die and wait for it to go ready */

  ret = rk3506_fspi_nand_op(SPINAND_RESET, 0, 0, 0, false, 0, NULL);
  if (ret >= 0)
    {
      ret = spinand_wait_ready(SPINAND_RESET_INIT_US,
                               SPINAND_RESET_POLL_US, NULL);
    }

  if (ret < 0)
    {
      ferr("ERROR: SPI NAND reset failed: %d\n", ret);
      kmm_free(priv->badstate);
      kmm_free(priv);
      return NULL;
    }

  /* Enable the on-die ECC engine (Linux spinand_ecc_enable(true)) */

  ret = spinand_ecc_enable(true);
  if (ret < 0)
    {
      ferr("ERROR: cannot enable on-die ECC: %d\n", ret);
      kmm_free(priv->badstate);
      kmm_free(priv);
      return NULL;
    }

  /* Fill in the MTD interface */

  priv->mtd.erase   = spinand_erase;
  priv->mtd.bread   = spinand_bread;
  priv->mtd.bwrite  = spinand_bwrite;
  priv->mtd.ioctl   = spinand_ioctl;
  priv->mtd.isbad   = spinand_isbad;
  priv->mtd.markbad = spinand_markbad;
  priv->mtd.name    = "rk_spinand";

  syslog(LOG_INFO,
         "SPI NAND: %s %lu KiB (%lu blocks x %lu KiB), page %u+%u, "
         "on-die ECC enabled\n",
         priv->id->name,
         (unsigned long)((uint32_t)priv->nblocks * priv->ppb *
                         priv->pagesize / 1024),
         (unsigned long)priv->nblocks,
         (unsigned long)(priv->ppb * priv->pagesize / 1024),
         priv->pagesize, priv->oobsize);

  return (FAR struct mtd_dev_s *)priv;
}
