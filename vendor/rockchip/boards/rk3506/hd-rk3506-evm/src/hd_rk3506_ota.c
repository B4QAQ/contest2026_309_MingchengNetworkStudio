/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_ota.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ota - NSH builtin for the A/B OTA slot manager (/dev/ota).
 *
 * Usage:
 *   ota status                show slot metadata + active slot
 *   ota bootcheck             burn one boot try of the active slot and
 *                             roll back when the last try is spent
 *                             (called from rcS on every boot)
 *   ota confirm               mark the current slot successful
 *   ota boot-a | boot-b       activate a slot (after its image was
 *                             verified); takes effect on next boot
 *   ota update <file> [-f]    erase the inactive slot, write <file>
 *                             (the boot.fit image), readback-verify and
 *                             activate it.  -f overrides the safety
 *                             checks (NOT recommended)
 *
 * Typical flow (with the new image at /data/nuttx.fit, e.g. copied from
 * a USB stick or fetched via curl):
 *   nsh> ota update /data/nuttx.fit
 *   nsh> reboot
 *   nsh> ota status            (slot B active, 0 tries burned)
 *   nsh> ota confirm           (make the new slot permanent)
 *
 * Rolling back: reboot without `ota confirm` 7 times (each boot burns a
 * try via rcS), or run `ota boot-a` manually.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "rk3506_ota.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define OTA_BUF_SIZE     65536
#define OTA_DEV          RK3506_OTA_DEVPATH

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void ota_print_status(FAR struct ota_status_s *st)
{
  int i;

  printf("OTA: metadata %s, last_boot=%c\n",
         st->meta_valid ? "valid" : "INVALID (U-Boot will reinit)",
         st->last_boot == 1 ? 'b' : 'a');

  for (i = 0; i < 2; i++)
    {
      printf("  slot_%c: priority=%2u tries=%u successful=%u fit=%s\n",
             'a' + i,
             st->slot[i].priority, st->slot[i].tries_remaining,
             st->slot[i].successful_boot,
             st->slot[i].fit_ok ? "yes" : "NO");
    }

  if (st->active_slot == 0xff)
    {
      printf("  active: NONE (no bootable slot!)\n");
    }
  else
    {
      printf("  active: slot_%c (U-Boot would boot this now)\n",
             'a' + st->active_slot);
    }
}

static int ota_get_status(int fd, FAR struct ota_status_s *st)
{
  int ret = ioctl(fd, OTAIOC_STATUS, (unsigned long)(uintptr_t)st);
  if (ret < 0)
    {
      fprintf(stderr, "ota: OTAIOC_STATUS failed: %d\n", errno);
    }

  return ret;
}

static int ota_cmd_update(int fd, int argc, FAR char **argv, bool force)
{
  FAR struct ota_status_s *st;
  FAR char *buf;
  const char *path;
  off_t filesize;
  ssize_t n;
  ssize_t written = 0;
  int srcfd;
  int chunk;
  int slot;

  if (argc < 1)
    {
      fprintf(stderr, "ota: update needs a file path\n");
      return EXIT_FAILURE;
    }

  path = argv[0];

  srcfd = open(path, O_RDONLY);
  if (srcfd < 0)
    {
      fprintf(stderr, "ota: open %s failed: %d\n", path, errno);
      return EXIT_FAILURE;
    }

  filesize = lseek(srcfd, 0, SEEK_END);
  lseek(srcfd, 0, SEEK_SET);
  if (filesize <= 0)
    {
      fprintf(stderr, "ota: %s is empty\n", path);
      close(srcfd);
      return EXIT_FAILURE;
    }

  st = malloc(sizeof(*st));
  buf = malloc(OTA_BUF_SIZE);
  if (st == NULL || buf == NULL)
    {
      fprintf(stderr, "ota: out of memory\n");
      free(st);
      free(buf);
      close(srcfd);
      return EXIT_FAILURE;
    }

  /* Which slot is active?  Write the OTHER one. */

  if (ota_get_status(fd, st) < 0)
    {
      free(st);
      free(buf);
      close(srcfd);
      return EXIT_FAILURE;
    }

  ota_print_status(st);

  slot = st->active_slot == 0 ? 1 : 0;
  printf("ota: writing %s (%ld bytes) to slot_%c\n",
         path, (long)filesize, 'a' + slot);

  /* 1. Erase the target slot (refused if it is the active one). */

  if (ioctl(fd, OTAIOC_UPDATE_BEGIN,
            (unsigned long)(slot | (force ? RK3506_OTA_FLAG_FORCE : 0))) < 0)
    {
      fprintf(stderr, "ota: UPDATE_BEGIN failed: %d\n", errno);
      free(st);
      free(buf);
      close(srcfd);
      return EXIT_FAILURE;
    }

  /* 2. Stream the image into the slot. */

  while ((n = read(srcfd, buf, OTA_BUF_SIZE)) > 0)
    {
      ssize_t w = write(fd, buf, n);
      if (w < 0)
        {
          fprintf(stderr, "ota: write to slot failed: %d\n", errno);
          close(srcfd);
          free(st);
          free(buf);
          return EXIT_FAILURE;
        }

      written += w;
    }

  if (n < 0)
    {
      fprintf(stderr, "ota: read %s failed: %d\n", path, errno);
      close(srcfd);
      free(st);
      free(buf);
      return EXIT_FAILURE;
    }

  close(srcfd);

  /* 3. Program the final partial page. */

  if (ioctl(fd, OTAIOC_UPDATE_COMMIT, (unsigned long)(uintptr_t)&written) < 0)
    {
      fprintf(stderr, "ota: UPDATE_COMMIT failed: %d\n", errno);
      free(st);
      free(buf);
      return EXIT_FAILURE;
    }

  printf("ota: wrote %ld bytes into slot_%c\n", (long)written, 'a' + slot);

  /* 4. Readback verify: compare the slot content byte for byte. */

  if (lseek(fd, 0, SEEK_SET) < 0)
    {
      fprintf(stderr, "ota: lseek failed: %d\n", errno);
      free(st);
      free(buf);
      return EXIT_FAILURE;
    }

  /* Reopen the source for the verify pass (srcfd was closed above). */

  srcfd = open(path, O_RDONLY);
  if (srcfd < 0)
    {
      fprintf(stderr, "ota: reopen %s failed: %d\n", path, errno);
      free(st);
      free(buf);
      return EXIT_FAILURE;
    }

  {
    off_t checked = 0;
    bool verify_ok = true;

    /* buf is OTA_BUF_SIZE bytes, split into two halves: source data at
     * the front, slot readback at the middle. */

    while (checked < filesize)
      {
        chunk = filesize - checked > OTA_BUF_SIZE / 2 ? OTA_BUF_SIZE / 2 :
                (int)(filesize - checked);
        n = read(srcfd, buf, chunk);
        if (n != chunk)
          {
            verify_ok = false;
            break;
          }

        if (read(fd, buf + OTA_BUF_SIZE / 2, chunk) != chunk)
          {
            fprintf(stderr, "ota: slot readback failed at %ld: %d\n",
                    (long)checked, errno);
            verify_ok = false;
            break;
          }

        if (memcmp(buf, buf + OTA_BUF_SIZE / 2, chunk) != 0)
          {
            fprintf(stderr, "ota: verify mismatch at offset %ld\n",
                    (long)checked);
            verify_ok = false;
            break;
          }

        checked += chunk;
      }

    if (!verify_ok)
      {
        fprintf(stderr, "ota: VERIFY FAILED - slot_%c NOT activated; "
                "the running slot is untouched\n", 'a' + slot);
        close(srcfd);
        free(st);
        free(buf);
        return EXIT_FAILURE;
      }
  }

  close(srcfd);
  printf("ota: readback verify OK (%ld bytes)\n", (long)filesize);

  /* 5. Activate the new slot. */

  if (ioctl(fd, OTAIOC_SET_ACTIVE,
            (unsigned long)(slot | (force ? RK3506_OTA_FLAG_FORCE : 0))) < 0)
    {
      fprintf(stderr, "ota: SET_ACTIVE failed: %d\n", errno);
      free(st);
      free(buf);
      return EXIT_FAILURE;
    }

  printf("ota: slot_%c activated (priority 15, tries 7).  "
         "Run `reboot` to boot it, then `ota confirm` to keep it.\n",
         'a' + slot);

  free(st);
  free(buf);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR struct ota_status_s st;
  int fd;
  int ret;

  if (argc < 2)
    {
      fprintf(stderr, "Usage: ota status|bootcheck|confirm|"
              "boot-a|boot-b|update <file> [-f]\n");
      return EXIT_FAILURE;
    }

  fd = open(OTA_DEV, O_RDWR);
  if (fd < 0)
    {
      fprintf(stderr, "ota: open %s failed: %d (OTA manager up?)\n",
              OTA_DEV, errno);
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "status") == 0)
    {
      ret = ota_get_status(fd, &st) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
      if (ret == EXIT_SUCCESS)
        {
          ota_print_status(&st);
        }
    }
  else if (strcmp(argv[1], "bootcheck") == 0)
    {
      struct ota_bootcheck_s bc;

      if (ioctl(fd, OTAIOC_BOOTCHECK, (unsigned long)(uintptr_t)&bc) < 0)
        {
          fprintf(stderr, "ota: BOOTCHECK failed: %d\n", errno);
          ret = EXIT_FAILURE;
        }
      else
        {
          switch (bc.action)
            {
              case 0:
                printf("ota: slot_%c already confirmed, no try burned\n",
                       'a' + bc.active_slot);
                break;
              case 1:
                printf("ota: slot_%c not confirmed yet, try burned "
                       "(%u left)\n", 'a' + bc.active_slot,
                       bc.tries_remaining);
                break;
              case 2:
                printf("ota: ROLLBACK: slot_%c tries exhausted, marked "
                       "unbootable; slot_%c restored as boot target\n",
                       'a' + bc.active_slot, 'a' + (bc.active_slot ^ 1));
                break;
              case 3:
                printf("ota: metadata was invalid, reinitialised "
                       "(slot_a default)\n");
                break;
              default:
                break;
            }

          ret = EXIT_SUCCESS;
        }
    }
  else if (strcmp(argv[1], "confirm") == 0)
    {
      int slot = -1;

      if (ioctl(fd, OTAIOC_CONFIRM, (unsigned long)(uintptr_t)&slot) < 0)
        {
          fprintf(stderr, "ota: CONFIRM failed: %d\n", errno);
          ret = EXIT_FAILURE;
        }
      else
        {
          printf("ota: slot_%c marked successful (permanent)\n",
                 'a' + slot);
          ret = EXIT_SUCCESS;
        }
    }
  else if (strcmp(argv[1], "boot-a") == 0)
    {
      if (ioctl(fd, OTAIOC_SET_ACTIVE, 0) < 0)
        {
          fprintf(stderr, "ota: SET_ACTIVE(a) failed: %d\n", errno);
          ret = EXIT_FAILURE;
        }
      else
        {
          printf("ota: slot_a activated\n");
          ret = EXIT_SUCCESS;
        }
    }
  else if (strcmp(argv[1], "boot-b") == 0)
    {
      if (ioctl(fd, OTAIOC_SET_ACTIVE, 1) < 0)
        {
          fprintf(stderr, "ota: SET_ACTIVE(b) failed: %d\n", errno);
          ret = EXIT_FAILURE;
        }
      else
        {
          printf("ota: slot_b activated\n");
          ret = EXIT_SUCCESS;
        }
    }
  else if (strcmp(argv[1], "update") == 0)
    {
      bool force = false;
      FAR char **upargv = &argv[2];
      int upargc = argc - 2;

      /* Skip an optional -f in front of or behind the path */

      if (upargc > 0 && strcmp(upargv[upargc - 1], "-f") == 0)
        {
          force = true;
          upargc--;
        }

      if (upargc > 0 && strcmp(upargv[0], "-f") == 0)
        {
          force = true;
          upargv++;
          upargc--;
        }

      ret = ota_cmd_update(fd, upargc, upargv, force);
    }
  else
    {
      fprintf(stderr, "ota: unknown command '%s'\n", argv[1]);
      ret = EXIT_FAILURE;
    }

  close(fd);
  return ret;
}
