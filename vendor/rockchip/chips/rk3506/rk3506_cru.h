/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_cru.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal Clock & Reset Unit (CRU) helpers for RK3506G2.
 *
 * The CRU base is 0xFF9A0000 (RK3506_CRU_ADDR in rk3506_memorymap.h).
 * Register banks (verified against the Linux clk driver
 * drivers/clk/rockchip/clk-rk3506.c and the SDK rk3506.h):
 *
 *   CLKSEL_CON(x)  = CRU + x*4 + 0x300   (clock mux / divider)
 *   CLKGATE_CON(x) = CRU + x*4 + 0x800   (clock enable / disable)
 *   SOFTRST_CON(x) = CRU + x*4 + 0xA00   (soft reset)
 *
 * Gate registers use the Rockchip HIWORD_UPDATE convention: the high
 * 16 bits are a per-bit write-enable mask and the low 16 bits carry the
 * value.  Gates additionally carry CLK_GATE_SET_TO_DISABLE semantics:
 * a value bit of 1 DISABLES the clock, 0 ENABLES (ungates) it.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_CRU_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_CRU_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_spi1_clock_init
 *
 * Description:
 *   Ungate the SPI1 clocks so the DesignWare SSI controller at
 *   0xFF130000 has a live APB register interface (PCLK_SPI1) and a
 *   functional clock (CLK_SPI1).
 *
 *   From the Linux clock tables (clk-rk3506.c):
 *     PCLK_SPI1 : GATE, CLKGATE_CON(12), bit 12
 *     CLK_SPI1  : COMPOSITE gate, CLKGATE_CON(12), bit 13
 *
 *   The mux/divider of CLK_SPI1 is left at its reset (BootROM/U-Boot)
 *   value; the DW SSI master derives its bit rate clock from the APB
 *   clock through the internal baud-rate generator, so ungating the two
 *   clocks is sufficient for register access and polling transfers.
 *
 *   The write is a read-mod-write-free HIWORD_UPDATE: only the two gate
 *   bits are touched, every other clock in CLKGATE_CON(12) is untouched.
 *
 ****************************************************************************/

void rk3506_spi1_clock_init(void);

/****************************************************************************
 * Name: rk3506_fspi_clock_init
 *
 * Description:
 *   Bring up the FSPI serial-flash controller clock (the on-board SPI
 *   NAND is on FSPI @0xFF488000 flash@0).  Muxes SCLK_FSPI to xin24m
 *   (24 MHz) with divider /1 and ungates HCLK_FSPI (CLKGATE_CON17 bit8)
 *   and SCLK_FSPI (CLKGATE_CON17 bit9).
 *
 ****************************************************************************/

void rk3506_fspi_clock_init(void);

/****************************************************************************
 * Name: rk3506_gmac0_clock_init
 *
 * Description:
 *   Bring up the GMAC0 (Synopsys DWMAC 4.20a) controller clock and
 *   deassert its reset, per the Linux SDK clk-rk3506.c table:
 *
 *     CLK_MAC_ROOT    : COMPOSITE_NOMUX, div[11:7] CLKSEL_CON(50),
 *                       gate CLKGATE_CON(17) bit 15, parent gpll
 *     CLK_MAC_PTP_ROOT: COMPOSITE,     mux[6:5] / div[4:0] CLKSEL_CON(55),
 *                       gate CLKGATE_CON(19) bit 9
 *     ACLK_MAC0       : GATE, CLKGATE_CON(17) bit 11
 *     PCLK_MAC0       : GATE, CLKGATE_CON(17) bit 13
 *     CLK_MAC0        : GATE, CLKGATE_CON(18) bit 0
 *     CLK_MAC0_PTP    : GATE, CLKGATE_CON(19) bit 10
 *     SRST_A_MAC0     : reset ID 283, SOFTRST_CON(17) bit 11
 *
 *   The PTP parent is forced to xin24m / 1 so PTP is functional even
 *   before a final reference clock is decided.  The MAC core clock
 *   (CLK_MAC_ROOT) is left at its default div /2 from GPLL (1.2 GHz
 *   / 2 = 150 MHz nominal).  After this call the MDIO bus is alive
 *   and the GMAC registers at 0xFF4C8000 are accessible.
 *
 ****************************************************************************/

void rk3506_gmac0_clock_init(void);

/****************************************************************************
 * Name: rk3506_pwm_clock_init
 *
 * Description:
 *   Ungate the PWM0 clocks (PCLK_PWM0 + CLK_PWM0, PMU CRU domain).
 *   The CLK_PWM0 divider stays at /1 so the PWM time base is the
 *   100 MHz clk_gpll_div_100m parent.
 *
 ****************************************************************************/

void rk3506_pwm_clock_init(void);

/****************************************************************************
 * Name: rk3506_saradc_clock_init
 *
 * Description:
 *   Ungate the SARADC clocks (PCLK_SARADC + CLK_SARADC), select the
 *   xin24m parent with div 15 (1.5 MHz converter clock) and release
 *   SRST_P_SARADC / SRST_SARADC.
 *
 ****************************************************************************/

void rk3506_saradc_clock_init(void);

/****************************************************************************
 * Name: rk3506_can_clock_init
 *
 * Description:
 *   Ungate the CAN interface clocks (HCLK + CLK, gpll/6 = 200 MHz)
 *   and release its resets.  iface: 0 = CAN0, 1 = CAN1.
 *
 ****************************************************************************/

void rk3506_can_clock_init(unsigned int iface);

/****************************************************************************
 * Name: rk3506_wdt_clock_init
 *
 * Description:
 *   Ungate one DW watchdog's clocks (PCLK + TCLK = xin24m 24 MHz).
 *   iface: 0 = WDT0, 1 = WDT1.
 *
 ****************************************************************************/

void rk3506_wdt_clock_init(unsigned int iface);

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_CRU_H */
