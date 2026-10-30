/****************************************************************************
 * vendor/rockchip/chips/rk3506/hardware/rk3506_gpio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO v2.2 register definitions for the Rockchip RK3506.
 * Taken from the Linux gpio-rockchip.c v2 driver and the CMSIS
 * struct GPIO_REG (rk3506.h:230-275).
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_GPIO_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_GPIO_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO v2.2 register offsets (from the bank base) */

#define GPIO_SWPORT_DR_L       0x00u
#define GPIO_SWPORT_DR_H       0x04u
#define GPIO_SWPORT_DDR_L      0x08u
#define GPIO_SWPORT_DDR_H      0x0cu
#define GPIO_INT_EN_L          0x10u
#define GPIO_INT_EN_H          0x14u
#define GPIO_INT_MASK_L        0x18u
#define GPIO_INT_MASK_H        0x1cu
#define GPIO_INT_TYPE_L        0x20u
#define GPIO_INT_TYPE_H        0x24u
#define GPIO_INT_POLARITY_L    0x28u
#define GPIO_INT_POLARITY_H    0x2cu
#define GPIO_INT_BOTHEDGE_L    0x30u
#define GPIO_INT_BOTHEDGE_H    0x34u
#define GPIO_DEBOUNCE_L        0x38u
#define GPIO_DEBOUNCE_H        0x3cu
#define GPIO_DBCLK_DIV_EN_L    0x40u
#define GPIO_DBCLK_DIV_EN_H    0x44u
#define GPIO_DBCLK_DIV_CON     0x48u
#define GPIO_INT_STATUS        0x50u
#define GPIO_INT_RAWSTATUS     0x58u
#define GPIO_PORT_EOI_L        0x60u
#define GPIO_PORT_EOI_H        0x64u
#define GPIO_EXT_PORT          0x70u
#define GPIO_VER_ID            0x78u

/* Interrupt trigger encodings (INT_TYPE / INT_POLARITY / BOTHEDGE),
 * matching gpio-rockchip.c rockchip_gpio_set_type():493-576.
 *   INT_TYPE     : 0 = level,      1 = edge
 *   INT_POLARITY : 0 = low/falling 1 = high/rising
 *   INT_BOTHEDGE : 1 = both edges (overrides polarity)
 */

#define RK3506_TRIG_LEVEL_HIGH   0u
#define RK3506_TRIG_LEVEL_LOW    1u
#define RK3506_TRIG_EDGE_RISING  2u
#define RK3506_TRIG_EDGE_FALLING 3u
#define RK3506_TRIG_EDGE_BOTH    4u

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_HARDWARE_RK3506_GPIO_H */
