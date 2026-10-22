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

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Visible GRAM window per the vendor demo (TFT_COLUMN_NUMBER /
 * TFT_LINE_NUMBER / TFT_COLUMN_OFFSET).
 */

#define NV3007_XRES          142
#define NV3007_YRES          428
#define NV3007_XOFFSET       12
#define NV3007_BPP           16

/* 4-wire SPI on DesignWare SPI0; conservative bring-up rate (the pad is
 * rated to 50 MHz, raise after validation).
 */

#define NV3007_SPI_BUS       0     /* rk3506_spibus_initialize() index    */
#define NV3007_SPI_FREQ      1000000
#define NV3007_SPI_MODE      0     /* SPIDEV_MODE0: CPOL=0, CPHA=0 */

/* J9 expansion-header pads, given as the GPIO0 in-bank pin number
 * (== RM_IO matrix physical index).  Only SCK/MOSI go through the SPI
 * controller; CS/DC/RST are ordinary GPIO outputs, matching the proven
 * vendor/Arduino bit-bang wiring (per-byte software CS):
 *   J9-17 SCK = GPIO0_A7 / RM_IO7 <- spi0_clk  (RMIO fn 82, mux 7)
 *   J9-16 SDA = GPIO0_A1 / RM_IO1 <- spi0_mosi (RMIO fn 83, mux 7)
 *   J9-14 CS  = GPIO0_A3 / RM_IO3  (plain GPIO output, idle high)
 *   J9-31 RST = GPIO0_B0 / RM_IO8  (plain GPIO output)
 *   J9-30 DC  = GPIO0_B3 / RM_IO11 (plain GPIO output)
 *   J9-29 BL  = GPIO0_B4 / RM_IO12 (PWM0 ch2, routed by rk3506_pwm)
 */

#define NV3007_PIN_CS        3     /* GPIO0_A3 -> J9-14                    */
#define NV3007_PIN_RST       8     /* GPIO0_B0 -> J9-31                    */
#define NV3007_PIN_DC        11    /* GPIO0_B3 -> J9-30                    */

#endif /* CONFIG_LCD_NV3007 */
#endif /* __BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_NV3007_H */
