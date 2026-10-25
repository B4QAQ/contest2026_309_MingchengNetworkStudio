/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_nv3007.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NV3007 2.79" SPI TFT panel (T279VJ-C10-01).  The driver implements the
 * NuttX lcd_dev_s lower-half; lcd_framebuffer exposes it as /dev/fb0.
 * Pin/init details come from the vendor demo
 * 2.79TFT-T279VJ-C10-01-SPI-NV3007.ino.
 ****************************************************************************/

#ifndef __BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_NV3007_H
#define __BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_NV3007_H

#include <nuttx/config.h>

#ifdef CONFIG_LCD_NV3007

#include <nuttx/spi/spi.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Visible GRAM window per the vendor demo (TFT_COLUMN_NUMBER /
 * TFT_LINE_NUMBER / TFT_COLUMN_OFFSET).  The controller GRAM is 168 wide;
 * the visible 142 columns start at X = 12.
 */

#define NV3007_XRES          142
#define NV3007_YRES          428
#define NV3007_XOFFSET       12
#define NV3007_BPP           16

/* 4-wire SPI on DesignWare SPI0.  The NV3007 is happy at 40 MHz on a short
 * ribbon; the RK3506 pad is rated 50 MHz.
 */

#define NV3007_SPI_BUS       0     /* rk3506_spibus_initialize() index    */

#ifndef NV3007_SPI_FREQ
#  define NV3007_SPI_FREQ    1000000
#endif

/* SPI mode.  MODE 0 (CPOL=0 / CPHA=0) is the mode verified by the upstream
 * ZMK nv3007 driver and is what every Arduino_GFX / TFT_eSPI port uses.
 */

#ifndef NV3007_SPI_MODE
#  define NV3007_SPI_MODE    SPIDEV_MODE0
#endif

/* CS / DC setup and hold, in microseconds.  The panel needs well under
 * 100 ns; 1 us is a 10x margin and costs nothing.  These guarantee that DC
 * is never switched while the controller is still shifting bytes out.
 */

#ifndef NV3007_CS_SETUP_US
#  define NV3007_CS_SETUP_US 1
#endif

#ifndef NV3007_CS_HOLD_US
#  define NV3007_CS_HOLD_US  1
#endif

#ifndef NV3007_DC_SETUP_US
#  define NV3007_DC_SETUP_US 1
#endif

/* J9 expansion-header pads.  The J9 SPI positions are hard-wired to
 * specific RM_IO channels (baseboard datasheet J9 table; SDK SPI.md:
 * CLK=rm_io1, MOSI=rm_io3, MISO=rm_io6, CSN=rm_io7):
 *   J9-17 SCK = GPIO0_A1 / RM_IO1  <- spi0_clk  (RMIO fn 82, mux 7)
 *   J9-16 SDA = GPIO0_A3 / RM_IO3  <- spi0_mosi (RMIO fn 83, mux 7)
 *   J9-14 CS  = GPIO0_A7 / RM_IO7  (plain GPIO output, idle high)
 *   J9-31 RST = GPIO0_B0 / RM_IO8  (plain GPIO output)
 *   J9-15 DC  = GPIO0_A6 / RM_IO6  (plain GPIO output)
 *   J9-29 BL  = GPIO0_B4 / RM_IO12 (plain GPIO output, high = lit)
 *
 * DC is on GPIO0_A6 (J9-15), the physical wiring verified with the
 * working Linux driver (vanxoak-hd-rk3506-evm-spi-nv3007.dtsi:
 * dc-gpios = <&gpio0 RK_PA6 GPIO_ACTIVE_HIGH>).  GPIO0_B3/J9-30 must
 * never be used here: on this board it is the gmac0 PHY hardware reset
 * (vanxoak-hd-rk3506-evm-v1.dtsi snps,reset-gpio = <&gpio0 RK_PB3>).
 */

#define NV3007_PIN_CS        7     /* GPIO0_A7 -> J9-14                    */
#define NV3007_PIN_RST       8     /* GPIO0_B0 -> J9-31                    */
#define NV3007_PIN_DC        6     /* GPIO0_A6 -> J9-15                    */
#define NV3007_PIN_BL        12    /* GPIO0_B4 -> J9-29                    */

#endif /* CONFIG_LCD_NV3007 */
#endif /* __BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_NV3007_H */
