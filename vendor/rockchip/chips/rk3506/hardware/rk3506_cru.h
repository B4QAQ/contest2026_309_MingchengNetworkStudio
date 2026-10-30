/****************************************************************************
 * vendor/rockchip/chips/rk3506/hardware/rk3506_cru.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CRU (Clock and Reset Unit) register definitions for the Rockchip RK3506.
 * Taken from the Linux clk-rk3506.c driver and the U-Boot cru_rk3506.h.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_CRU_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_CRU_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CRU register layout (offsets from CRU base 0xff9a0000) */

#define RK3506_CRU_MODE_CON        0x0280u
#define RK3506_CRU_CLKSEL_CON(x)   ((x) * 4u + 0x0300u)
#define RK3506_CRU_CLKGATE_CON(x)  ((x) * 4u + 0x0800u)
#define RK3506_CRU_SOFTRST_CON(x)  ((x) * 4u + 0x0a00u)
#define RK3506_CRU_GLB_CNT_TH      0x0c00u
#define RK3506_CRU_GLB_RST_ST      0x0c04u
#define RK3506_CRU_GLB_SRST_FST    0x0c08u
#define RK3506_CRU_GLB_SRST_SND    0x0c0cu
#define RK3506_CRU_GLB_RST_CON     0x0c10u

/* PMU CRU register layout (offsets from PMU CRU base 0xff9b0000) */

#define RK3506_PMUCRU_CLKSEL_CON(x)  ((x) * 4u + 0x0300u)
#define RK3506_PMUCRU_CLKGATE_CON(x) ((x) * 4u + 0x0800u)
#define RK3506_PMUCRU_SOFTRST_CON(x) ((x) * 4u + 0x0a00u)

/* Clock gate bits (from Linux clk-rk3506.c) */

/* PMU_CLKGATE_CON(0) */

#define PMU_CLKGATE0_PCLK_PMU        (1u << 5)
#define PMU_CLKGATE0_PCLK_GPIO0_IOC  (1u << 7)
#define PMU_CLKGATE0_PCLK_GPIO0      (1u << 8)
#define PMU_CLKGATE0_DBCLK_GPIO0     (1u << 9)
#define PMU_CLKGATE0_PCLK_PWM0       (1u << 15)

/* CLKGATE_CON(3) */

#define CLKGATE3_PCLK_GPIO1_IOC      (1u << 7)
#define CLKGATE3_PCLK_GPIO1          (1u << 8)
#define CLKGATE3_DBCLK_GPIO1         (1u << 9)

/* CLKGATE_CON(12) */

#define CLKGATE12_PCLK_SPI0          (1u << 10)
#define CLKGATE12_CLK_SPI0           (1u << 11)
#define CLKGATE12_PCLK_SPI1          (1u << 12)
#define CLKGATE12_CLK_SPI1           (1u << 13)
#define CLKGATE12_PCLK_GPIO2         (1u << 14)
#define CLKGATE12_DBCLK_GPIO2        (1u << 15)

/* CLKGATE_CON(13) */

#define CLKGATE13_PCLK_GPIO3         (1u << 0)
#define CLKGATE13_DBCLK_GPIO3        (1u << 1)
#define CLKGATE13_PCLK_GPIO4         (1u << 2)
#define CLKGATE13_DBCLK_GPIO4        (1u << 3)

/* CLKGATE_CON(19) */

#define CLKGATE19_PCLK_GPIO234_IOC   (1u << 8)

/* CLKGATE_CON(22) */

#define CLKGATE22_PCLK_GPIO1_IOC     (1u << 1)

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_CRU_H */
