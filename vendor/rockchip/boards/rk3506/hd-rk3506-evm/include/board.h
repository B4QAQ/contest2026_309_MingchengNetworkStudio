/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/include/board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-specific definitions for HD-RK3506-EVM.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_INCLUDE_BOARD_H
#define __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Board identification */

#define BOARD_NAME "HD-RK3506-EVM"
#define BOARD_VENDOR "Rockchip"

/* Clocking *****************************************************************/

/* The HD-RK3506-EVM board features a single 24MHz crystal. */

#define BOARD_XTAL_FREQUENCY   24000000ul

/* Console UART (UART0, J9 pins 22/23) */

#define BOARD_UART0_BAUD       115200
#define BOARD_UART0_BITS       8
#define BOARD_UART0_PARITY     0
#define BOARD_UART0_STOP       2

/* GPIO pin definitions (J9 expansion header)
 *
 * The J9 header carries GPIO0_RM_IO[0..15] pins.  Pin numbers below are
 * the in-bank pin number (== RM_IO matrix physical index).
 */

/* NV3007 2.79" SPI TFT (J9) */

#define BOARD_NV3007_PIN_CS    3     /* GPIO0_A3 -> J9-14 */
#define BOARD_NV3007_PIN_RST   8     /* GPIO0_B0 -> J9-31 */
#define BOARD_NV3007_PIN_DC    11    /* GPIO0_B3 -> J9-30 */
#define BOARD_NV3007_PIN_BL    12    /* GPIO0_B4 -> J9-29 */
#define BOARD_NV3007_PIN_CLK   7     /* GPIO0_A7 -> J9-17 (RM_IO, fn 82) */
#define BOARD_NV3007_PIN_MOSI  1     /* GPIO0_A1 -> J9-16 (RM_IO, fn 83) */

/* LED definitions (if available) */

#ifdef CONFIG_USERLED
#  define BOARD_NLEDS 2
#  define BOARD_LED_PWR 0
#  define BOARD_LED_RUN 1
#endif

/* Heartbeat LED (SDK: GPIO1_B3, active low) */

#define BOARD_LED_HEARTBEAT_BANK  1
#define BOARD_LED_HEARTBEAT_PIN   11    /* GPIO1_B3 */
#define BOARD_LED_HEARTBEAT_ACTIVE_LOW

/* UART pin definitions (SDK dts)
 *
 * UART1: RM_IO16 (RX), RM_IO17 (TX) -> GPIO1_B0/B1
 * UART2: RM_IO0 (RX), RM_IO2 (TX)  -> GPIO0_A0/A2 (M0 debug)
 * UART3: RM_IO15 (RX), RM_IO14 (TX) -> GPIO0_B7/B6
 */

#define BOARD_UART1_RX_PIN      16    /* RM_IO16 -> GPIO1_B0 */
#define BOARD_UART1_TX_PIN      17    /* RM_IO17 -> GPIO1_B1 */
#define BOARD_UART2_RX_PIN      0     /* RM_IO0  -> GPIO0_A0 */
#define BOARD_UART2_TX_PIN      2     /* RM_IO2  -> GPIO0_A2 */
#define BOARD_UART3_RX_PIN      15    /* RM_IO15 -> GPIO0_B7 */
#define BOARD_UART3_TX_PIN      14    /* RM_IO14 -> GPIO0_B6 */

/* I2C pin definitions (SDK dts)
 *
 * I2C0: RM_IO25 (SCL), RM_IO24 (SDA) -> GPIO1_B9/B8
 * I2C2: RM_IO4 (SCL), RM_IO5 (SDA)   -> GPIO0_A4/A5
 */

#define BOARD_I2C0_SCL_PIN      25    /* RM_IO25 -> GPIO1_B9 */
#define BOARD_I2C0_SDA_PIN      24    /* RM_IO24 -> GPIO1_B8 */
#define BOARD_I2C2_SCL_PIN      4     /* RM_IO4  -> GPIO0_A4 */
#define BOARD_I2C2_SDA_PIN      5     /* RM_IO5  -> GPIO0_A5 */

/* SD/MMC pin definitions (SDK dts)
 *
 * CD: GPIO3_A6 (card detect, active low)
 */

#define BOARD_MMC_CD_BANK       3
#define BOARD_MMC_CD_PIN        6     /* GPIO3_A6 */

/* GMAC0 PHY reset (SDK dts) */

#define BOARD_GMAC0_PHY_RESET_BANK  0
#define BOARD_GMAC0_PHY_RESET_PIN   11  /* GPIO0_B3 -> J9-30 */

/* GMAC1 PHY reset (SDK dts) */

#define BOARD_GMAC1_PHY_RESET_BANK  0
#define BOARD_GMAC1_PHY_RESET_PIN   12  /* GPIO0_B4 -> J9-29 */

/* PWM0 backlight (SDK dts) */

#define BOARD_PWM0_CH2_PIN      10    /* RM_IO10 -> GPIO0_B2 */

/****************************************************************************
 * Public Functions Definitions
 ****************************************************************************/

#ifndef __ASSEMBLY__

int hd_rk3506_bringup(void);

#ifdef CONFIG_GRAPHICS_LVGL
int hd_rk3506_lv_port_disp_init(void);
#endif

#ifdef CONFIG_IOEXPANDER
int hd_rk3506_gpio_initialize(void);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_INCLUDE_BOARD_H */
