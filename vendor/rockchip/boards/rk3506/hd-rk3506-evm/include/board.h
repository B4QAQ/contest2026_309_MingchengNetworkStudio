/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/include/board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-specific definitions for HD-RK3506-EVM.
 *
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

/* LED definitions (if available) */

#ifdef CONFIG_USERLED
#  define BOARD_NLEDS 2
#  define BOARD_LED_PWR 0
#  define BOARD_LED_RUN 1
#endif

/* Console UART */

#define BOARD_UART0_BAUD 115200
#define BOARD_UART0_BITS 8
#define BOARD_UART0_PARITY 0
#define BOARD_UART0_STOP 2

#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_INCLUDE_BOARD_H */
