/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_iomux.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-fixed pin-mux setup for the RK3506G2 peripherals used on the
 * HD-RK3506-EVM.
 *
 * This file is now a thin, per-peripheral wrapper around the unified
 * pin-control back-end (rk3506_pinctrl.c).  All register-level work
 * (the per-bank IOC base, the 4-bit IOMUX nibble, the RM_IO routing
 * matrix, pull/drive) lives in rk3506_pinctrl_set_mux(); here we only
 * pass the (bank, pin, function) triples taken from the U-Boot pinctrl
 * DTS (rk3506-pinctrl.dtsi / rk3506-pinctrl-rmio.dtsi).
 *
 * Keeping these named setup functions (rather than open-coding the
 * triples in each driver) documents which pads each peripheral owns on
 * this board and gives the drivers a single call site.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rk3506_pinctrl.h"
#include "rk3506_iomux.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Alternate function numbers (rk3506-pinctrl.dtsi).  For the RM_IO
 * pins (I2C0) the value is the DTS "mux" number (>15); for the
 * directly-muxed peripherals it is the plain 4-bit function select.
 */

#define SPI1_FUNC          2u   /* <0 RK_PBx 2 ...>            */
#define FSPI_FUNC          1u   /* <2 RK_PAx 1 ...>            */
#define GMAC0_RMII_FUNC    1u   /* all RMII0 pins share func 1 */

/* I2C0 RM_IO function numbers (rk3506-pinctrl-rmio.dtsi):
 *   rm_io24_i2c0_sda = <1 RK_PB1 31>   (bank1 pin 9,  func 31)
 *   rm_io25_i2c0_scl = <1 RK_PB2 30>   (bank1 pin 10, func 30)
 */

#define RMIO_MUX_I2C0_SDA  31u
#define RMIO_MUX_I2C0_SCL  30u

/* I2C2 RM_IO function numbers (rk3506-pinctrl-rmio.dtsi):
 *   rm_io4_i2c2_scl = <0 RK_PA4 32>    (bank0 pin 4, func 32)
 *   rm_io5_i2c2_sda = <0 RK_PA5 33>    (bank0 pin 5, func 33)
 */

#define RMIO_MUX_I2C2_SCL  32u
#define RMIO_MUX_I2C2_SDA  33u

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_ioc_spi1_setup
 *
 * Description:
 *   Mux GPIO0 PB0/PB1/PB2/PB6 to SPI1 function 2 (clk/mosi/miso/csn0),
 *   per the U-Boot pinctrl DTS (rk3506-pinctrl.dtsi spi1 node).
 *
 ****************************************************************************/

void rk3506_ioc_spi1_setup(void)
{
  rk3506_pinctrl_set_mux(0, 8,  SPI1_FUNC);   /* PB0 SCLK  */
  rk3506_pinctrl_set_mux(0, 9,  SPI1_FUNC);   /* PB1 MOSI  */
  rk3506_pinctrl_set_mux(0, 10, SPI1_FUNC);   /* PB2 MISO  */
  rk3506_pinctrl_set_mux(0, 14, SPI1_FUNC);   /* PB6 CSN0  */
}

/****************************************************************************
 * Name: rk3506_ioc_fspi_setup
 *
 * Description:
 *   Switch the six GPIO2 port-A pins used by FSPI (alt function 1) to
 *   their FSPI function (csn/clk/d0-d3), per the U-Boot pinctrl DTS
 *   (rk3506-pinctrl.dtsi fspi node).
 *
 ****************************************************************************/

void rk3506_ioc_fspi_setup(void)
{
  uint8_t pin;

  /* PA0 csn, PA1 clk, PA2 d0, PA3 d1, PA4 d2, PA5 d3 */

  for (pin = 0; pin <= 5; pin++)
    {
      rk3506_pinctrl_set_mux(2, pin, FSPI_FUNC);
    }
}

/****************************************************************************
 * Name: rk3506_ioc_i2c0_setup
 *
 * Description:
 *   Route RM_IO24/25 to I2C0 SDA/SCL, per the SDK pinctrl data
 *   (rk3506-pinctrl-rmio.dtsi):
 *
 *     rm_io24_i2c0_sda: <1 RK_PB1 31 &pcfg_pull_none>
 *     rm_io25_i2c0_scl: <1 RK_PB2 30 &pcfg_pull_none>
 *
 *   Both function numbers exceed the 4-bit IOMUX field, so
 *   rk3506_pinctrl_set_mux() programs the RM_IO routing channel
 *   (function - 15) and forces the GPIO1 IOMUX nibble to the matrix
 *   path (7) -- using the dedicated GPIO1 IOC1 block at 0xFF660000.
 *   (The previous version wrote the nibble into the shared IOC_GRF
 *   space, which is the GPIO1-addressing bug fixed by the unified
 *   back-end.)
 *
 *   The GT911 touch controller sits on this bus; without this setup
 *   GPIO1_B1/B2 stay in func 0 (GPIO) and every I2C0 transfer NAKs.
 *
 ****************************************************************************/

void rk3506_ioc_i2c0_setup(void)
{
  rk3506_pinctrl_set_mux(1, 9,  RMIO_MUX_I2C0_SDA);  /* B1 -> RM_IO24 */
  rk3506_pinctrl_set_mux(1, 10, RMIO_MUX_I2C0_SCL);  /* B2 -> RM_IO25 */
}

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

void rk3506_ioc_i2c2_setup(void)
{
  rk3506_pinctrl_set_mux(0, 4,  RMIO_MUX_I2C2_SCL);  /* A4 -> RM_IO4 */
  rk3506_pinctrl_set_mux(0, 5,  RMIO_MUX_I2C2_SDA);  /* A5 -> RM_IO5 */
}

/****************************************************************************
 * Name: rk3506_ioc_gmac0_rmii_setup
 *
 * Description:
 *   Switch the nine GPIO2 port-B/C pins used by GMAC0 RMII to their
 *   alt function 1, per the U-Boot pinctrl DTS
 *   (rk3506-pinctrl.dtsi eth_rmii0 node):
 *
 *     PB0 rxd0,  PB1 rxd1,    PC0 rxdvcrs,
 *     PB3 txd0,  PB4 txd1,    PB5 txen,
 *     PB2 rmii_clk,           PB6 mdc,  PB7 mdio
 *
 ****************************************************************************/

void rk3506_ioc_gmac0_rmii_setup(void)
{
  /* PB0..PB7 -> rxd0/rxd1/rmii_clk/txd0/txd1/txen/mdc/mdio */

  rk3506_pinctrl_set_mux(2, 8,  GMAC0_RMII_FUNC);  /* PB0 rxd0     */
  rk3506_pinctrl_set_mux(2, 9,  GMAC0_RMII_FUNC);  /* PB1 rxd1     */
  rk3506_pinctrl_set_mux(2, 10, GMAC0_RMII_FUNC);  /* PB2 rmii_clk */
  rk3506_pinctrl_set_mux(2, 11, GMAC0_RMII_FUNC);  /* PB3 txd0     */
  rk3506_pinctrl_set_mux(2, 12, GMAC0_RMII_FUNC);  /* PB4 txd1     */
  rk3506_pinctrl_set_mux(2, 13, GMAC0_RMII_FUNC);  /* PB5 txen     */
  rk3506_pinctrl_set_mux(2, 14, GMAC0_RMII_FUNC);  /* PB6 mdc      */
  rk3506_pinctrl_set_mux(2, 15, GMAC0_RMII_FUNC);  /* PB7 mdio     */

  /* PC0 rxdvcrs */

  rk3506_pinctrl_set_mux(2, 16, GMAC0_RMII_FUNC);  /* PC0 rxdvcrs  */
}
