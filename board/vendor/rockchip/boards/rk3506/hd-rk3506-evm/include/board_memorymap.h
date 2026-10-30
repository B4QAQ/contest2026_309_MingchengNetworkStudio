/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/include/board_memorymap.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-specific memory map definitions for HD-RK3506-EVM.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_INCLUDE_BOARD_MEMORYMAP_H
#define __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_INCLUDE_BOARD_MEMORYMAP_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RAM layout (from ld.script) */

#define BOARD_RAM_START        0x02080000
#define BOARD_RAM_SIZE         0x08000000  /* 128 MB */

/* Peripheral bases (from chip memorymap) */

#define BOARD_GPIO0_BASE       0xff940000
#define BOARD_GPIO1_BASE       0xff870000
#define BOARD_GPIO2_BASE       0xff1c0000
#define BOARD_GPIO3_BASE       0xff1d0000
#define BOARD_GPIO4_BASE       0xff1e0000

#define BOARD_SPI0_BASE        0xff120000
#define BOARD_SPI1_BASE        0xff130000

#define BOARD_UART0_BASE       0xff0a0000
#define BOARD_UART1_BASE       0xff0b0000
#define BOARD_UART2_BASE       0xff0c0000

#define BOARD_I2C0_BASE        0xff160000
#define BOARD_I2C1_BASE        0xff170000
#define BOARD_I2C2_BASE        0xff180000

#define BOARD_GMAC0_BASE       0xff600000

#define BOARD_FSPI_BASE        0xff488000

#define BOARD_USB_OTG1_BASE    0xff780000

#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_INCLUDE_BOARD_MEMORYMAP_H */
