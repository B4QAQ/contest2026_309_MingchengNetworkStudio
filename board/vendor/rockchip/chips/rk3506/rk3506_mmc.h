/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_mmc.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506 SD/MMC host controller (Synopsys DesignWare MSHC, "dw-mshc")
 * at 0xFF480000, GIC SPI 86.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_MMC_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_MMC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#include <nuttx/sdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3506_mmc_initialize
 *
 * Description:
 *   Clock, configure and register the RK3506 SD/MMC controller as NuttX
 *   MMCSD slot `slot` (/dev/mmcsd<slot>).  Calls mmcsd_slotinitialize()
 *   internally, so the caller only needs to have run the board pin/power
 *   setup beforehand.
 *
 * Input Parameters:
 *   slot - MMCSD slot number (0 for the single controller)
 *
 * Returned Value:
 *   OK on success, or -ENODEV if no card is present (the slot is still
 *   registered and hotplug is armed), or another negated errno on a
 *   hard failure.
 *
 ****************************************************************************/

int rk3506_mmc_initialize(int slot);

/****************************************************************************
 * Name: rk3506_mmc_carddetect
 *
 * Description:
 *   Poll the card-detect source and, if the insertion state changed and
 *   the MMCSD upper half has armed SDIOMEDIA_INSERTED/EJECTED, schedule
 *   the media-change callback on the high-priority work queue.
 *
 *   Called by board-level poll logic (there is no CD interrupt on this
 *   board).  Safe to call from a task; does nothing after the callback
 *   has been disabled again by the upper half.
 *
 * Returned Value:
 *   true if a card is currently present, false otherwise.
 *
 ****************************************************************************/

bool rk3506_mmc_carddetect(void);

/****************************************************************************
 * Name: rk3506_mmc_sdio
 *
 * Description:
 *   Return the controller's sdio_dev_s instance (NULL before
 *   rk3506_mmc_initialize()).  For board code that needs SDIO_* access.
 *
 ****************************************************************************/

FAR struct sdio_dev_s *rk3506_mmc_sdio(void);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_MMC_H */
