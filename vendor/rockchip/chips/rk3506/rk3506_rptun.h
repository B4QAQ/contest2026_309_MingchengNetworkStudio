/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_rptun.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506G2 A7-side rptun lower half for the bus Cortex-M0 ("mcu").
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_RPTUN_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_RPTUN_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3506_rptun_initialize
 *
 * Description:
 *   Register the RK3506 bus-M0 rptun device.  The lower half is the
 *   rpmsg master: U-Boot already loaded the M0 firmware FIT into DRAM
 *   0x03a00000 and released the core before NuttX started, so start()
 *   only ungates the clocks U-Boot leaves closed and probes the peer for
 *   readiness; the driver then services the mailbox0/mailbox3 kick
 *   interrupts.  With is_autostart = true the rptun core
 *   spawns its worker thread from rptun_initialize() and the rpmsg
 *   virtio device exposes /dev/rpmsg/mcu once probed.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3506_rptun_initialize(void);

/* Dump all A7-readable M0 mailbox / INTMUX / vring state to syslog. */

void rk3506_rptun_diag(void);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_RPTUN_H */
