/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_iomux.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal IOMUX (pin function select) helpers for RK3506G2.
 *
 * GPIO bank 0 (the low-power/PMU IO bank) mux registers live in the
 * "ioc_pmu" register block at 0xFF950000 (RK3506_IOC_PMU_ADDR, also
 * GPIO0_IOC_BASE in the SDK rk3506.h).  This is confirmed by:
 *
 *   - the U-Boot DTS node  ioc_pmu: syscon@ff950000
 *   - the pinctrl driver using regmap_pmu for bank 0
 *     (pinctrl-rk3506.c rk3506_set_mux: bank 0 -> regmap_pmu)
 *   - struct GPIO0_IOC_REG in the SDK CMSIS header
 *
 * Mux registers use the Rockchip HIWORD_UPDATE convention: the high
 * half-word is a per-nibble write-enable mask, the low half-word holds
 * the 4-bit function select per pin (one nibble per pin).
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_IOMUX_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_IOMUX_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_ioc_spi1_setup
 *
 * Description:
 *   Switch the four GPIO0 pins used by SPI1 (alt function 2) to their
 *   SPI function, per the U-Boot pinctrl DTS (rk3506-pinctrl.dtsi):
 *
 *     spi1_clk  : bank0 RK_PB0 (pin  8) -> func 2
 *     spi1_mosi : bank0 RK_PB1 (pin  9) -> func 2
 *     spi1_miso : bank0 RK_PB2 (pin 10) -> func 2
 *     spi1_csn0 : bank0 RK_PB6 (pin 14) -> func 2
 *
 *   Bank0 sub-bank B mux register layout (struct GPIO0_IOC_REG):
 *     GPIO0B_IOMUX_SEL_0 @ 0x08 : pins  8, 9,10,11 in nibbles 0,1,2,3
 *     GPIO0B_IOMUX_SEL_1 @ 0x0C : pins 12,13,14,15 in nibbles 0,1,2,3
 *
 *   Each pin's 4-bit nibble is programmed with an isolated HIWORD_UPDATE
 *   write, so the other three pins sharing the same register are
 *   untouched.
 *
 ****************************************************************************/

void rk3506_ioc_spi1_setup(void);

/****************************************************************************
 * Name: rk3506_ioc_fspi_setup
 *
 * Description:
 *   Switch the six GPIO2 port-A pins used by FSPI (alt function 1) to
 *   their FSPI function (csn/clk/d0-d3) in the ioc_grf block at
 *   0xFF4D8000, per the U-Boot pinctrl DTS fspi node.
 *
 ****************************************************************************/

void rk3506_ioc_fspi_setup(void);

/****************************************************************************
 * Name: rk3506_ioc_i2c0_setup
 *
 * Description:
 *   Route RM_IO24/25 to I2C0 SDA/SCL and set GPIO1_B1/B2 to RM_IO
 *   function (see rk3506_iomux.c for the full register sequence).
 *   Required before any I2C0 transfer (GT911 touch).
 *
 ****************************************************************************/

void rk3506_ioc_i2c0_setup(void);

/****************************************************************************
 * Name: rk3506_ioc_gmac0_rmii_setup
 *
 * Description:
 *   Switch the nine GPIO2 port-B/C pins used by GMAC0 RMII (rxd0/rxd1/
 *   rxdvcrs/txd0/td1/txen/rmii_clk + mdc/mdio) to alt function 1, per
 *   the U-Boot pinctrl DTS eth_rmii0 node.  The 50 MHz RMII reference
 *   clock source is configured later in GRF_SOC_CON8, not here.
 *
 ****************************************************************************/

void rk3506_ioc_gmac0_rmii_setup(void);

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_IOMUX_H */
