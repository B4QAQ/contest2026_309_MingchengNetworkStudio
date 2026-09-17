/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_ota.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506 A/B OTA slot manager (/dev/ota).
 *
 * Implements the Rockchip / Android A/B boot-control protocol consumed by
 * the prebuilt U-Boot (CONFIG_ANDROID_AB):
 *
 *   - Slot metadata is a 32-byte AvbABData struct stored at offset 2048 of
 *     the "misc" MTD partition (u-boot lib/avb/libavb_ab/avb_ab_flow.c:
 *     AB_METADATA_MISC_PARTITION_OFFSET).  Stored in network byte order,
 *     CRC32 (IEEE reflected, poly 0xEDB88320, init/xorout ~0) over the
 *     first 28 bytes.
 *   - U-Boot's boot_fit picks the bootable slot with the highest priority
 *     (priority > 0 and (successful_boot || tries_remaining > 0)); ties
 *     favour slot A (index 0).
 *   - If the metadata is missing/invalid, U-Boot re-initialises it to the
 *     defaults (slot A priority 15 / tries 7 / successful 0; slot B
 *     priority 14 / tries 7 / successful 0) and boots boot_a.
 *   - Partition lookup: U-Boot maps the "boot" name to "boot_a"/"boot_b"
 *     using the slot suffix, falling back to "boot" if neither exists.
 *
 * The driver exposes /dev/ota with the ioctls below so an NSH builtin can
 * inspect the slots, stream an image into the inactive slot, verify it,
 * flip the metadata and implement the boot-try decrement/rollback that the
 * U-Boot boot_fit path does not do by itself.
 *
 * Slot / partition mapping (nand_firmware/parameter.txt, 512B sectors):
 *   boot_a: 0x6800 .. 0xb800   (13 MB .. 23 MB)
 *   boot_b: 0xb800 .. 0x10800  (23 MB .. 33 MB)
 *   misc  : 0x5800 .. 0x6800   (11 MB .. 13 MB)
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_OTA_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_OTA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/mtd/mtd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Device registered by rk3506_ota_initialize() */

#define RK3506_OTA_DEVPATH    "/dev/ota"

/* Number of slots (A=0, B=1) and misc metadata geometry */

#define RK3506_OTA_NSLOTS     2
#define RK3506_OTA_META_SIZE  32
#define RK3506_OTA_META_OFF   2048

/* Private ioctl base (vendor-local, outside the NuttX-assigned bases) */

#define _RK3506_OTAIOC(n)     (0x5a00 + (n))

#define OTAIOC_STATUS         _RK3506_OTAIOC(1)   /* arg: struct ota_status_s*  */
#define OTAIOC_BOOTCHECK      _RK3506_OTAIOC(2)   /* arg: struct ota_bootcheck_s* */
#define OTAIOC_CONFIRM        _RK3506_OTAIOC(3)   /* arg: int* current slot     */
#define OTAIOC_SET_ACTIVE     _RK3506_OTAIOC(4)   /* arg: int slot (0/1)        */
#define OTAIOC_UPDATE_BEGIN   _RK3506_OTAIOC(5)   /* arg: int slot (0/1)        */
#define OTAIOC_UPDATE_COMMIT  _RK3506_OTAIOC(6)   /* arg: ssize_t* written      */
#define OTAIOC_SELECT         _RK3506_OTAIOC(7)   /* arg: int slot (0/1) -> read() source */

/* OTAIOC_SET_ACTIVE / OTAIOC_UPDATE_BEGIN flag: OR 0x100 into the slot
 * argument to force the operation even when the safety checks (target has
 * a valid FIT header / target is not the active slot) fail.  Only for
 * development; a normal OTA flow never needs it. */

#define RK3506_OTA_FLAG_FORCE 0x100

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One slot, as reported to userspace */

struct ota_slotinfo_s
{
  uint8_t priority;        /* 0 = unbootable .. 15 = highest */
  uint8_t tries_remaining; /* boot attempts left before rollback */
  uint8_t successful_boot; /* nonzero once marked successful */
  uint8_t fit_ok;          /* slot starts with the FIT magic 0xd00dfeed */
};

/* OTAIOC_STATUS payload */

struct ota_status_s
{
  uint8_t  meta_valid;        /* AvbABData magic+CRC verified */
  uint8_t  active_slot;       /* slot U-Boot would boot now (0/1, 0xff if none) */
  uint8_t  last_boot;         /* last_boot byte from the metadata */
  uint8_t  reserved;
  struct ota_slotinfo_s slot[RK3506_OTA_NSLOTS];
};

/* OTAIOC_BOOTCHECK payload: the boot-try bookkeeping run once per boot
 * from rcS (U-Boot's boot_fit path does not decrement tries itself).
 * action: 0 = slot already successful, nothing to do,
 *         1 = tries decremented,
 *         2 = tries exhausted -> slot marked unbootable, other slot raised
 *         3 = metadata was missing/invalid -> reinitialised to defaults */

struct ota_bootcheck_s
{
  uint8_t active_slot;
  uint8_t action;
  uint8_t tries_remaining;
  uint8_t reserved;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#if defined(CONFIG_RK3506_OTA)

/****************************************************************************
 * Name: rk3506_ota_initialize
 *
 * Description:
 *   Create the misc / boot_a / boot_b MTD partitions from the full-chip
 *   SPI NAND MTD and register the /dev/ota character device implementing
 *   the A/B slot protocol.
 *
 * Input Parameters:
 *   mtd - The full-chip MTD from rk3506_spinand_fspi_initialize().
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3506_ota_initialize(FAR struct mtd_dev_s *mtd);

#endif /* CONFIG_RK3506_OTA */

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_OTA_H */
