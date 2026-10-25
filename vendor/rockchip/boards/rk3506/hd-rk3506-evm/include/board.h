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
