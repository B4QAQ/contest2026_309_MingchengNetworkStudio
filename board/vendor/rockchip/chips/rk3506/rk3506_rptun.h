/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_rptun.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Definitions for the RK3506 bus-M0 rptun lower half (rk3506_rptun.c).
 *
 * The rpmsg link to the M0 ("mcu", link-id 0x03 = RL_PLATFORM_SET_LINK_ID
 * (0, 3), the MASTER_ID/REMOTE_ID pair of the SDK hal rpmsg test) uses:
 *   - mailbox3 A2B for A7 -> MCU kicks (the MCU receives it via INTMUX BB_3)
 *   - mailbox0 B2A (GIC SPI 138) for MCU tvq kicks -> A7
 *   - mailbox3 B2A (GIC SPI 141) for MCU rvq kicks -> A7
 * Shared memory is the 2 MiB window at pa 0x03c00000 (identity to the
 * MCU's LINUX_RPMSG linker region): vrings at 0x03c00000 / 0x03c08000,
 * buffer-pool carveout at 0x03c10000.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_RPTUN_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_RPTUN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Shared memory window (must match the MCU's gcc_bus_m0.ld LINUX_RPMSG
 * region and the vring layout of the SDK rpmsg platform) */

#define RK3506_RPMSG_SHMEM_BASE   0x03c00000u
#define RK3506_RPMSG_SHMEM_SIZE   0x00200000u
#define RK3506_RPMSG_VRING0_DA    0x03c00000u
#define RK3506_RPMSG_VRING1_DA    0x03c08000u
#define RK3506_RPMSG_CARVEOUT_DA  0x03c10000u
#define RK3506_RPMSG_CARVEOUT_LEN 0x1f0000u
#define RK3506_RPMSG_VRING_NUM    64u
#define RK3506_RPMSG_VRING_ALIGN  0x1000u

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#if defined(CONFIG_RK3506_RPTUN)

/****************************************************************************
 * Name: rk3506_rptun_initialize
 *
 * Description:
 *   Register the RK3506 bus-M0 rptun device (/dev/rptun/mcu) and start
 *   it (autostart).  Boots the M0 with the embedded rpmsg-test firmware
 *   and brings up the rpmsg master (the /dev/rpmsg/mcu node appears once
 *   the virtio rpmsg driver has probed the device).
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3506_rptun_initialize(void);

#endif /* CONFIG_RK3506_RPTUN */

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_RPTUN_H */
