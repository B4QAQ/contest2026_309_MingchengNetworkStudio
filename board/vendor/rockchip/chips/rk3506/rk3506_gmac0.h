/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_gmac0.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public API for the RK3506 GMAC0 (DWMAC 4.20a) NuttX Ethernet driver.
 *
 * M2b.3: the driver is a full netdev built on the Linux SDK GMAC HAL
 * ported verbatim in rk3506_gmac_hal.c (hal_gmac.c + hal_gmac_rk3506.c).
 * The SoC/board glue (CRU clocks in rk3506_cru.c, RMII IOMUX in
 * rk3506_iomux.c, GPIO4_A2 PHY reset per the HD-RK3506-EVM board doc)
 * follows the verified M2b.1/M2b.2 bring-up.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_GMAC0_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_GMAC0_H

#include <stdint.h>
#include <nuttx/compiler.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/* Register the GMAC0 netdev ("eth0") with the network stack.  Called
 * once from board bring-up (hd_rk3506_bringup.c).  Hardware power-up
 * happens later in the d_ifup() callback, driven by netdev/ifup or
 * the NETINIT daemon (netinit + DHCPC in the nsh defconfig).
 *
 * Returns OK on success, negated errno on failure.
 */

int rk3506_gmac0_initialize(void);

/* Copy the eth0 cumulative byte counters (NET_STATISTICS).  Flat-build
 * helper for board apps (hd_ui network rate card): the counters live
 * in the driver's private net_driver_s and userspace has no standard
 * ioctl path to them.  Returns -ENODEV when eth0 is not registered.
 */

int rk3506_gmac0_get_stats(FAR uint64_t *rx_bytes, FAR uint64_t *tx_bytes);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_GMAC0_H */
