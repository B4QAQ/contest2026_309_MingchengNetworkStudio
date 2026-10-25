/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_iomux.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-fixed pin-mux setup helpers for the RK3506G2.
 *
 * These are thin, per-peripheral wrappers around the unified
 * pin-control back-end (rk3506_pinctrl.h).  All register-level detail
 * (per-bank IOC base, 4-bit IOMUX nibble, RM_IO routing matrix,
 * pull/drive) lives in rk3506_pinctrl_set_mux(); the functions here
 * only pass the (bank, pin, function) triples from the U-Boot pinctrl
 * DTS and document which pads each peripheral owns on the board.
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
 *   The nibble-level register layout is handled internally by
 *   rk3506_pinctrl_set_mux().
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
 * Name: rk3506_ioc_i2c2_setup
 *
 * Description:
 *   Route RM_IO4/5 to I2C2 SCL/SDA, per the SDK pinctrl data
 *   (rk3506-pinctrl-rmio.dtsi):
 *
 *     rm_io4_i2c2_scl:  <0 RK_PA4 32 &pcfg_pull_none>
 *     rm_io5_i2c2_sda:  <0 RK_PA5 33 &pcfg_pull_none>
 *
 *   The rjgt102 crypt chip sits on this bus at address 0x68.
 *
 ****************************************************************************/

void rk3506_ioc_i2c2_setup(void);

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
