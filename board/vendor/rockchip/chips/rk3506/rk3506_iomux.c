/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_iomux.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal IOMUX (pin function select) support for RK3506G2.
 *
 * Implements the SPI1, FSPI and GMAC0 pin muxes.  Register base, bank
 * offsets and the per-pin nibble write are taken directly from the
 * U-Boot Rockchip pinctrl driver (drivers/pinctrl/rockchip/
 * pinctrl-rk3506.c rk3506_pin_banks[] + pinctrl-rockchip-core.c
 * rockchip_get_mux_data()) and the DTS pin tables (rk3506-pinctrl.dtsi):
 *
 *   reg    = bank->iomux[pin/8].offset            (see bank table below)
 *   if ((pin % 8) >= 4) reg += 0x4                (IOMUX_WIDTH_4BIT)
 *   bit    = (pin % 4) * 4                        (4-bit field per pin)
 *   mask   = 0xf
 *   data   = (0xf << (bit + 16)) | (mux & 0xf) << bit
 *
 * Bank iomux base offsets (PIN_BANK_IOMUX_FLAGS_OFFSET, per 8 pins):
 *   gpio0: 0x0 / 0x8 / 0x10 / 0x830   (ioc_pmu @ 0xFF950000)
 *   gpio1: 0x20 / 0x28 / 0x30 / 0x38  (ioc_grf @ 0xFF4D8000)
 *   gpio2: 0x40 / 0x48 / 0x50 / 0x58  (ioc_grf @ 0xFF4D8000)
 *   gpio3: 0x60 / 0x68 / 0x70 / 0x78  (ioc_grf @ 0xFF4D8000)
 *
 * So the concrete GMAC0 RMII pin registers (ioc_grf base):
 *   GPIO2B0-3  -> 0x48        GPIO2B4-7  -> 0x4C   (MDC=PB6, MDIO=PB7!)
 *   GPIO2C0-3  -> 0x50
 * and FSPI pins (GPIO2A0-5):
 *   GPIO2A0-3  -> 0x40        GPIO2A4-7  -> 0x44
 *
 * BUG HISTORY (M2b.1): the GMAC0/FSPI setup used 0x20/0x24/0x28/0x2c/0x30,
 * which is the GPIO1 bank space - the GMAC pins were never muxed away
 * from GPIO function, so the PHY never saw MDC and every MDIO read
 * returned 0.  FSPI only worked because its pins are the boot device
 * and the BootROM muxes them at reset.  The wrong writes also left
 * GPIO1 A/B/C pins muxed to func 1; this file now restores them.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_iomux.h"

/* CRU-independent register access (fall back if the arch headers do
 * not provide putreg32/getreg32).
 */

#ifndef putreg32
#  define putreg32(v, a) (*(FAR volatile uint32_t *)(a) = (v))
#endif

#ifndef getreg32
#  define getreg32(a)    (*(FAR volatile uint32_t *)(a))
#endif

/* GPIO0 (ioc_pmu) mux registers for sub-bank B (pins 8..15).
 * IOMUX_WIDTH_4BIT: four pins per 32-bit register, one 4-bit function
 * select nibble per pin; pins 4..7 of the sub-bank live at +4:
 *   GPIO0B_IOMUX @ 0x08 : pin 8 nibble0, pin 9 nibble1,
 *                         pin10 nibble2, pin11 nibble3
 *   GPIO0B+0x04 @ 0x0C : pin12 nibble0, pin13 nibble1,
 *                         pin14 nibble2, pin15 nibble3
 */

#define IOC_GPIO0B_SEL_0   (RK3506_IOC_PMU_ADDR + 0x08u)
#define IOC_GPIO0B_SEL_1   (RK3506_IOC_PMU_ADDR + 0x0Cu)

/* GPIO2 (ioc_grf @ 0xFF4D8000) mux registers.  Bank offsets 0x40/0x48/
 * 0x50/0x58 for A/B/C/D, each split into two 4-pin registers:
 */

#define IOC_GPIO2A_SEL_0   (RK3506_IOC_GRF_ADDR + 0x40u)  /* PA0..PA3  */
#define IOC_GPIO2A_SEL_1   (RK3506_IOC_GRF_ADDR + 0x44u)  /* PA4..PA7  */
#define IOC_GPIO2B_SEL_0   (RK3506_IOC_GRF_ADDR + 0x48u)  /* PB0..PB3  */
#define IOC_GPIO2B_SEL_1   (RK3506_IOC_GRF_ADDR + 0x4cu)  /* PB4..PB7  */
#define IOC_GPIO2C_SEL_0   (RK3506_IOC_GRF_ADDR + 0x50u)  /* PC0..PC3  */

/* GPIO1 (ioc_grf @ 0xFF4D8000) mux registers for sub-bank B.
 * IOMUX_WIDTH_4BIT: one 4-bit function nibble per pin:
 *   GPIO1B_IOMUX @ 0x28 : pin B0 nibble0 (bit 0),  B1 nibble1 (bit 4),
 *                         B2 nibble2 (bit 8),  B3 nibble3 (bit 12)
 */

#define IOC_GPIO1B_SEL_0    (RK3506_IOC_GRF_ADDR + 0x28u)

/* RM_IO routing registers.  RK3506_GRF_PMU_ADDR (0xff910000) comes from
 * hardware/rk3506_memorymap.h.  When a pinctrl function number exceeds
 * the 4-bit iomux field (>15) the pin is an RM_IO-matrix pin: the real
 * routing function is (func - 15) and is programmed into grf_pmu +
 * 0xbc + 4*pin (bank 1, pins 9..11), then the standard iomux nibble
 * must ALSO be set to func 7.  Sequence per u-boot pinctrl-rk3506.c
 * rockchip_set_rmio() and Linux pinctrl-rockchip.c (both identical for
 * RK3506).
 */

#define RMIO_BANK1_REG(pin) (RK3506_GRF_PMU_ADDR + 0xBCu + 0x4u * (pin))
#define RMIO_BANK1_DATA(fn) (0x7f0000u | ((fn) & 0x7fu))
#define RMIO_IOMUX_FUNC     7u   /* nibble value after RM_IO routing */

/* RM_IO function numbers (rk3506-pinctrl-rmio.dtsi):
 *   rm_io24_i2c0_sda = <1 RK_PB1 31>  -> routing fn 31-15 = 16
 *   rm_io25_i2c0_scl = <1 RK_PB2 30>  -> routing fn 30-15 = 15
 */

#define RMIO_FN_I2C0_SDA    16u
#define RMIO_FN_I2C0_SCL    15u

/* Alternate function numbers (rk3506-pinctrl.dtsi) */

#define SPI1_FUNC          2u   /* <0 RK_PBx 2 ...>            */
#define FSPI_FUNC          1u   /* <2 RK_PAx 1 ...>            */
#define GMAC0_RMII_FUNC    1u   /* all RMII0 pins share func 1 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_ioc_mux_set
 *
 * Description:
 *   Program one 4-bit pin function select nibble using the Rockchip
 *   HIWORD_UPDATE convention (high half-word = write-enable mask).
 *
 ****************************************************************************/

static void rk3506_ioc_mux_set(uint32_t reg, uint32_t bit, uint32_t func)
{
  uint32_t data = (0xfu << (bit + 16)) | ((func & 0xfu) << bit);

  putreg32(data, reg);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_ioc_spi1_setup
 *
 * Description:
 *   Mux GPIO0 PB0/PB1/PB2/PB6 to SPI1 function 2 (clk/mosi/miso/csn0).
 *
 ****************************************************************************/

void rk3506_ioc_spi1_setup(void)
{
  /* PB0=pin8 SCLK : SEL_0 nibble0 (bit 0)
   * PB1=pin9 MOSI : SEL_0 nibble1 (bit 4)
   * PB2=pin10 MISO: SEL_0 nibble2 (bit 8)
   */

  rk3506_ioc_mux_set(IOC_GPIO0B_SEL_0, 0, SPI1_FUNC);  /* PB0 SCLK  */
  rk3506_ioc_mux_set(IOC_GPIO0B_SEL_0, 4, SPI1_FUNC);  /* PB1 MOSI  */
  rk3506_ioc_mux_set(IOC_GPIO0B_SEL_0, 8, SPI1_FUNC);  /* PB2 MISO  */

  /* PB6=pin14 CSN0 : +0x04 register nibble2 (bit 8) */

  rk3506_ioc_mux_set(IOC_GPIO0B_SEL_1, 8, SPI1_FUNC);  /* PB6 CSN0  */
}

/****************************************************************************
 * Name: rk3506_ioc_fspi_setup
 *
 * Description:
 *   Switch the six GPIO2 port-A pins used by FSPI (alt function 1) to
 *   their FSPI function, per the U-Boot pinctrl DTS
 *   (rk3506-pinctrl.dtsi fspi { ... }):
 *
 *     fspi_csn : bank2 RK_PA0 (pin 0) -> func 1
 *     fspi_clk : bank2 RK_PA1 (pin 1) -> func 1
 *     fspi_d0  : bank2 RK_PA2 (pin 2) -> func 1
 *     fspi_d1  : bank2 RK_PA3 (pin 3) -> func 1
 *     fspi_d2  : bank2 RK_PA4 (pin 4) -> func 1
 *     fspi_d3  : bank2 RK_PA5 (pin 5) -> func 1
 *
 *   NOTE: these pins are the boot-device pins and the BootROM already
 *   muxes them to FSPI at reset, so the previous version's writes to
 *   the WRONG registers (0x20/0x24 = GPIO1 space) went unnoticed -
 *   FSPI worked without any iomux programming.  This version writes
 *   the correct GPIO2A registers.
 *
 ****************************************************************************/

void rk3506_ioc_fspi_setup(void)
{
  /* PA0 CSn, PA1 CLK, PA2 D0, PA3 D1 : SEL_0 nibbles 0..3 */

  rk3506_ioc_mux_set(IOC_GPIO2A_SEL_0,  0, FSPI_FUNC);  /* PA0 csn */
  rk3506_ioc_mux_set(IOC_GPIO2A_SEL_0,  4, FSPI_FUNC);  /* PA1 clk */
  rk3506_ioc_mux_set(IOC_GPIO2A_SEL_0,  8, FSPI_FUNC);  /* PA2 d0  */
  rk3506_ioc_mux_set(IOC_GPIO2A_SEL_0, 12, FSPI_FUNC);  /* PA3 d1  */

  /* PA4 D2, PA5 D3 : SEL_1 nibbles 0..1 */

  rk3506_ioc_mux_set(IOC_GPIO2A_SEL_1,  0, FSPI_FUNC);  /* PA4 d2  */
  rk3506_ioc_mux_set(IOC_GPIO2A_SEL_1,  4, FSPI_FUNC);  /* PA5 d3  */
}

/****************************************************************************
 * Name: rk3506_ioc_i2c0_setup
 *
 * Description:
 *   Route the RM_IO24/25 matrix pins to I2C0 SDA/SCL and switch the
 *   underlying GPIO1_B1/GPIO1_B2 pins to RM_IO function 7, per the
 *   SDK pinctrl data:
 *
 *     rm_io24_i2c0_sda: <1 RK_PB1 31 &pcfg_pull_none>
 *     rm_io25_i2c0_scl: <1 RK_PB2 30 &pcfg_pull_none>
 *
 *   Because 31/30 exceed the 4-bit iomux field, these are RM_IO
 *   matrix pins: grf_pmu (0xFF910000) gets the routing function
 *   (func - 15) at 0xbc + 4*pin and the GPIO1B iomux nibble is then
 *   set to 7 (rk3506_set_rmio() in u-boot pinctrl-rk3506.c).
 *
 *   NOTE (BUG HISTORY M4.1): the GT911 touch controller sits on this
 *   bus; without this setup GPIO1_B1/B2 stayed in func 0 (GPIO) and
 *   every I2C0 transfer NAKed (GT911 probe: -110 ETIMEDOUT).
 *
 ****************************************************************************/

void rk3506_ioc_i2c0_setup(void)
{
  /* 1. RM_IO routing: which peripheral owns RM_IO24/25.
   *    SDA = RM_IO24 = GPIO1 pin 9  (0xbc + 4*9 = 0xE0), fn 16
   *    SCL = RM_IO25 = GPIO1 pin 10 (0xbc + 4*10 = 0xE4), fn 15
   */

  putreg32(RMIO_BANK1_DATA(RMIO_FN_I2C0_SDA), RMIO_BANK1_REG(9));
  putreg32(RMIO_BANK1_DATA(RMIO_FN_I2C0_SCL), RMIO_BANK1_REG(10));

  /* 2. Standard iomux nibbles: GPIO1_B1 -> nibble1 (bit 4),
   *    GPIO1_B2 -> nibble2 (bit 8), both func 7.
   */

  rk3506_ioc_mux_set(IOC_GPIO1B_SEL_0, 4, RMIO_IOMUX_FUNC);  /* B1 SDA */
  rk3506_ioc_mux_set(IOC_GPIO1B_SEL_0, 8, RMIO_IOMUX_FUNC);  /* B2 SCL */
}

/****************************************************************************
 * Name: rk3506_ioc_gmac0_rmii_setup
 *
 * Description:
 *   Switch the nine GPIO2 port-B/C pins used by GMAC0 RMII to their
 *   alt function 1, per the U-Boot pinctrl DTS
 *   (rk3506-pinctrl.dtsi eth_rmii0 { ... }):
 *
 *     PB0 rxd0,  PB1 rxd1,    PC0 rxdvcrs,
 *     PB3 txd0,  PB4 txd1,    PB5 txen,
 *     PB2 rmii_clk,           PB6 mdc,  PB7 mdio
 *
 *   Register mapping (IOMUX_WIDTH_4BIT, bank gpio2 @ ioc_grf):
 *     PB0..PB3 -> 0x48 nibbles 0..3
 *     PB4..PB7 -> 0x48+4 = 0x4C nibbles 0..3   (PB6=MDC, PB7=MDIO)
 *     PC0..PC3 -> 0x50 nibbles 0..3
 *
 *   This function also undoes the earlier misdirected writes that
 *   landed in GPIO1 space (see BUG HISTORY in the file header).
 *
 ****************************************************************************/

void rk3506_ioc_gmac0_rmii_setup(void)
{
  /* NOTE: an earlier version of this file wrote the WRONG registers
   * (GPIO1 bank space 0x20/0x24/0x28/0x2c/0x30) and then "undid" the
   * damage by clearing whole GPIO1 registers on every call.  The mux
   * bug was fixed (all GMAC0 pins live in GPIO2 space, below) and the
   * blanket clear was REMOVED: with I2C0 now legitimately using
   * GPIO1_B1/B2 (RM_IO matrix, func 7 in register 0x28), a blanket
   * clear here would silently kill the touch controller's IOMUX on
   * every ifup.
   */

  /* PB0 rxd0, PB1 rxd1, PB2 rmii_clk, PB3 txd0 : SEL_0 nibbles 0..3 */

  rk3506_ioc_mux_set(IOC_GPIO2B_SEL_0,  0, GMAC0_RMII_FUNC);  /* PB0 rxd0    */
  rk3506_ioc_mux_set(IOC_GPIO2B_SEL_0,  4, GMAC0_RMII_FUNC);  /* PB1 rxd1    */
  rk3506_ioc_mux_set(IOC_GPIO2B_SEL_0,  8, GMAC0_RMII_FUNC);  /* PB2 rmii_clk*/
  rk3506_ioc_mux_set(IOC_GPIO2B_SEL_0, 12, GMAC0_RMII_FUNC);  /* PB3 txd0    */

  /* PB4 txd1, PB5 txen, PB6 mdc, PB7 mdio : SEL_1 (0x4C) nibbles 0..3 */

  rk3506_ioc_mux_set(IOC_GPIO2B_SEL_1,  0, GMAC0_RMII_FUNC);  /* PB4 txd1    */
  rk3506_ioc_mux_set(IOC_GPIO2B_SEL_1,  4, GMAC0_RMII_FUNC);  /* PB5 txen    */
  rk3506_ioc_mux_set(IOC_GPIO2B_SEL_1,  8, GMAC0_RMII_FUNC);  /* PB6 mdc     */
  rk3506_ioc_mux_set(IOC_GPIO2B_SEL_1, 12, GMAC0_RMII_FUNC);  /* PB7 mdio    */

  /* PC0 rxdvcrs : SEL_0 nibble 0 */

  rk3506_ioc_mux_set(IOC_GPIO2C_SEL_0,  0, GMAC0_RMII_FUNC);  /* PC0 rxdvcrs */
}
