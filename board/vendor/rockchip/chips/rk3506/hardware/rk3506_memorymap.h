/****************************************************************************
 * vendor/rockchip/chips/rk3506/hardware/rk3506_memorymap.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_MEMORYMAP_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_MEMORYMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GIC (Generic Interrupt Controller v2) Base Addresses */

#define RK3506_GIC_DIST_BASE     0xff581000  /* GIC Distributor */
#define RK3506_GIC_CPU_BASE      0xff582000  /* GIC CPU Interface */

/* UART Base Addresses (DesignWare 8250 compatible)
 * reg-shift = <2>, reg-io-width = <4>
 */

#define RK3506_UART0_ADDR        0xff0a0000  /* Debug UART, IRQ 34 */
#define RK3506_UART1_ADDR        0xff0b0000  /* IRQ 35 */
#define RK3506_UART2_ADDR        0xff0c0000  /* IRQ 36 */
#define RK3506_UART3_ADDR        0xff0d0000  /* IRQ 37 */
#define RK3506_UART4_ADDR        0xff0e0000  /* IRQ 38 */
#define RK3506_UART5_ADDR        0xff4e0000  /* IRQ 39 */

/* GPIO Base Addresses */

#define RK3506_GPIO0_ADDR        0xff940000  /* IRQ 0-3 */
#define RK3506_GPIO1_ADDR        0xff870000  /* IRQ 4-7 */
#define RK3506_GPIO2_ADDR        0xff1c0000  /* IRQ 8-11 */
#define RK3506_GPIO3_ADDR        0xff1d0000  /* IRQ 12-15 */
#define RK3506_GPIO4_ADDR        0xff1e0000  /* IRQ 16-19 */

/* I2C Base Addresses */

#define RK3506_I2C0_ADDR         0xff040000
#define RK3506_I2C1_ADDR         0xff050000
#define RK3506_I2C2_ADDR         0xff060000

/* SPI Base Addresses */

#define RK3506_SPI0_ADDR         0xff120000  /* IRQ 43 */
#define RK3506_SPI1_ADDR         0xff130000  /* IRQ 44 */
#define RK3506_FSPI_ADDR         0xff488000  /* IRQ 85 */

/* USB Controller (DWC2) Base Addresses */

#define RK3506_USBOTG0_ADDR      0xff740000  /* IRQ 74, DWC2 OTG */
#define RK3506_USBOTG1_ADDR      0xff780000  /* IRQ 79, DWC2 OTG */
#define RK3506_USB2PHY_ADDR      0xff2b0000  /* USB2 PHY */

/* Display Controller */

#define RK3506_VOP_ADDR          0xff600000  /* IRQ 59, VOP */
#define RK3506_DSI_ADDR          0xff640000  /* IRQ 60, MIPI DSI */
#define RK3506_DSI_DPHY_ADDR     0xff670000  /* MIPI DSI DPHY */

/* PWM Base Addresses */

#define RK3506_PWM0_4CH_0_ADDR   0xff930000  /* IRQ 22, PMU domain */
#define RK3506_PWM0_4CH_1_ADDR   0xff931000  /* IRQ 23 */
#define RK3506_PWM0_4CH_2_ADDR   0xff932000  /* IRQ 24 */
#define RK3506_PWM0_4CH_3_ADDR   0xff933000  /* IRQ 25 */
#define RK3506_PWM1_8CH_0_ADDR   0xff170000  /* IRQ 26, BUS domain */
#define RK3506_PWM1_8CH_1_ADDR   0xff171000  /* IRQ 27 */
#define RK3506_PWM1_8CH_2_ADDR   0xff172000  /* IRQ 28 */
#define RK3506_PWM1_8CH_3_ADDR   0xff173000  /* IRQ 29 */

/* System Registers */

#define RK3506_CRU_ADDR          0xff9a0000  /* Clock Reset Unit */
#define RK3506_GRF_ADDR          0xff288000  /* General Register Files */
#define RK3506_GRF_PMU_ADDR      0xff910000  /* PMU GRF */
#define RK3506_IOC_GRF_ADDR      0xff4d8000  /* IOC GRF (Pin Control) */
#define RK3506_IOC1_ADDR         0xff660000  /* IOC1 */
#define RK3506_IOC_PMU_ADDR      0xff950000  /* IOC PMU */

/* DMA Controllers */

#define RK3506_DMAC0_ADDR        0xff000000  /* IRQ 116/117, PL330 */
#define RK3506_DMAC1_ADDR        0xff008000  /* IRQ 118/119, PL330 */

/* Watchdog Timers */

#define RK3506_WDT0_ADDR         0xff260000  /* IRQ 107, DW WDT */
#define RK3506_WDT1_ADDR         0xff268000  /* IRQ 108, DW WDT */

/* Ethernet (GMAC) */

#define RK3506_GMAC0_ADDR        0xff4c8000  /* IRQ 66/69 */
#define RK3506_GMAC1_ADDR        0xff4d0000  /* IRQ 70/73 */

/* Other Peripherals */

#define RK3506_CAN0_ADDR         0xff320000  /* IRQ 45 */
#define RK3506_CAN1_ADDR         0xff330000  /* IRQ 46 */
#define RK3506_SDMMC_ADDR        0xff480000  /* IRQ 86 */
#define RK3506_SARADC_ADDR       0xff4e8000  /* IRQ 57 */
#define RK3506_TSADC_ADDR        0xff650000  /* IRQ 58 */
#define RK3506_CRYPTO_ADDR       0xff700000  /* IRQ 111 */
#define RK3506_RNG_ADDR          0xff710000  /* IRQ 114 */
#define RK3506_FLEXBUS_ADDR      0xff880000  /* IRQ 127 */
#define RK3506_DSMC_ADDR         0xff8b0000  /* IRQ 126 */
#define RK3506_MAILBOX0_ADDR     0xff290000  /* IRQ 138 */
#define RK3506_HWSPINLOCK0_ADDR  0xff240000

/* DDR Memory */

#define RK3506_DDR_BASE          0x00000000  /* DDR start address */
#define RK3506_DDR_SIZE          0x08000000  /* 128MB */

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_MEMORYMAP_H */
