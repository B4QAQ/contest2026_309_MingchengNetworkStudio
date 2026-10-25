/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_gpio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO controller driver for the Rockchip RK3506G2.
 *
 * Two usage models are provided, both sitting on the same register
 * back-end:
 *
 *   1. A NuttX ioexpander lower-half (CONFIG_IOEXPANDER).  One
 *      ioexpander instance per GPIO bank is registered; the board code
 *      then calls gpio_lower_half() per exported pin to create the
 *      /dev/gpioN character devices (the R528-style model).
 *
 *   2. A small set of chip-level helpers (rk3506_gpio_config /
 *      rk3506_gpio_write / rk3506_gpio_read) used by on-board drivers
 *      that need to wiggle a dedicated pin (LCD reset / DC, Ethernet
 *      PHY reset, ...) without going through the character-device
 *      interface.  This replaces the private putreg32() bit-banging
 *      that used to be duplicated in hd_rk3506_nv3007.c and
 *      rk3506_gmac0.c.
 *
 * The RK3506 GPIO controller is the Rockchip GPIO v2.2 IP (VER_ID
 * 0x010219C8), register-compatible with the Linux gpio-rockchip.c v2
 * layout.  All data/direction/interrupt registers are 16-bit-split
 * (low/high half-word) with a HIWORD write-enable: a write carries the
 * per-bit write-enable mask in the high half-word and the data in the
 * low half-word, so single pins can be updated without a
 * read-modify-write.
 *
 * Register map (offsets from the bank base, gpio-rockchip.c:92-107 and
 * CMSIS struct GPIO_REG rk3506.h:230-275):
 *
 *   SWPORT_DR_L      0x00    SWPORT_DR_H      0x04
 *   SWPORT_DDR_L     0x08    SWPORT_DDR_H      0x0C
 *   INT_EN_L         0x10    INT_EN_H         0x14
 *   INT_MASK_L       0x18    INT_MASK_H       0x1C
 *   INT_TYPE_L       0x20    INT_TYPE_H       0x24
 *   INT_POLARITY_L   0x28    INT_POLARITY_H   0x2C
 *   INT_BOTHEDGE_L   0x30    INT_BOTHEDGE_H   0x34
 *   DEBOUNCE_L       0x38    DEBOUNCE_H       0x3C
 *   DBCLK_DIV_EN_L   0x40    DBCLK_DIV_EN_H   0x44
 *   DBCLK_DIV_CON    0x48
 *   INT_STATUS       0x50    INT_RAWSTATUS    0x58
 *   PORT_EOI_L       0x60    PORT_EOI_H       0x64
 *   EXT_PORT         0x70    VER_ID           0x78
 *
 * Bank bases (rk3506_memorymap.h / rk3506.h:2217-2298):
 *   GPIO0 0xFF940000 (PMU domain)   GPIO1 0xFF870000
 *   GPIO2 0xFF1C0000                GPIO3 0xFF1D0000
 *   GPIO4 0xFF1E0000
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_GPIO_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_GPIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

#include <nuttx/ioexpander/ioexpander.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Encode/decode a global pin number: bank in bits [6:5], pin in [4:0].
 * Global pin numbers match the Linux gpio-ranges (bank*32 + pin), so
 * GPIO1_B1 = 32 + 9 = 41.
 */

#define RK3506_GPIO_PIN(bank, pin)   ((uint8_t)(((bank) << 5) | (pin)))
#define RK3506_GPIO_BANK(gpin)       ((uint8_t)((gpin) >> 5))
#define RK3506_GPIO_PINNO(gpin)      ((uint8_t)((gpin) & 0x1fu))

#define RK3506_NR_GPIOS              160u  /* 5 banks x 32 pins */

/* Configuration flags for rk3506_gpio_config() (bit field) */

#define RK3506_GPIO_INPUT            0x01u  /* input (Hi-Z)            */
#define RK3506_GPIO_OUTPUT           0x02u  /* push-pull output        */
#define RK3506_GPIO_VALUE_LOW        0x00u  /* initial output low (default) */
#define RK3506_GPIO_VALUE_HIGH       0x04u  /* initial output high     */
#define RK3506_GPIO_PULL_UP          0x08u  /* enable pull-up          */
#define RK3506_GPIO_PULL_DOWN        0x10u  /* enable pull-down        */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3506_gpio_config
 *
 * Description:
 *   Chip-level helper: mux a pin to GPIO function, ungate its bank
 *   clock, and set its direction / initial level / pull in one call.
 *   Intended for board drivers that own a dedicated pin.
 *
 * Input Parameters:
 *   gpin  - Global pin number (RK3506_GPIO_PIN(bank, pin))
 *   flags - Bitwise OR of RK3506_GPIO_* configuration flags.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno on failure.
 *
 ****************************************************************************/

int rk3506_gpio_config(uint8_t gpin, uint32_t flags);

/****************************************************************************
 * Name: rk3506_gpio_write
 *
 * Description:
 *   Drive an output pin high or low (HIWORD single-bit write to
 *   SWPORT_DR_L/H).
 *
 ****************************************************************************/

int rk3506_gpio_write(uint8_t gpin, bool value);

/****************************************************************************
 * Name: rk3506_gpio_read
 *
 * Description:
 *   Sample a pin's pad level (EXT_PORT).  Works for both input and
 *   output pins.
 *
 ****************************************************************************/

int rk3506_gpio_read(uint8_t gpin, FAR bool *value);

/****************************************************************************
 * Name: rk3506_gpio_bank_initialize
 *
 * Description:
 *   Initialize one GPIO bank as a NuttX ioexpander lower-half device:
 *   ungate the bank clocks and install the ioexpander ops.  The
 *   returned device is then handed to gpio_lower_half() by board code
 *   to create /dev/gpioN nodes.
 *
 *   This function is idempotent per bank.
 *
 * Input Parameters:
 *   bank - GPIO bank number, 0..4
 *
 * Returned Value:
 *   A pointer to the bank's ioexpander device on success; NULL on
 *   failure (invalid bank or CONFIG_IOEXPANDER disabled).
 *
 ****************************************************************************/

#ifdef CONFIG_IOEXPANDER
FAR struct ioexpander_dev_s *rk3506_gpio_bank_initialize(uint8_t bank);
#endif

/****************************************************************************
 * Name: rk3506_gpio_irq_attach / rk3506_gpio_irq_enable
 *
 * Description:
 *   Chip-level GPIO interrupt support (used by the ioexpander attach /
 *   option paths and available to board drivers).  gpio_irq_attach
 *   routes the pin's interrupt to the GIC handler and records the
 *   per-pin callback; gpio_irq_enable arms/disarms it.
 *
 *   The trigger type is programmed by the ioexpander IOEXPANDER_OPTION_
 *   INTCFG path (rk3506_ioe_option) or by board code through
 *   rk3506_gpio_irq_config().
 *
 ****************************************************************************/

#ifdef CONFIG_IOEXPANDER_INT_ENABLE
int rk3506_gpio_irq_config(uint8_t gpin, uint32_t trigger);
int rk3506_gpio_irq_attach(uint8_t gpin, ioe_callback_t callback,
                           FAR void *arg);
int rk3506_gpio_irq_enable(uint8_t gpin, bool enable);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_GPIO_H */
