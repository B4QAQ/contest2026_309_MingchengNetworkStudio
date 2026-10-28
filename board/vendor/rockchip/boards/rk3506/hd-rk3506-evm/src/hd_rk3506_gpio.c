/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_gpio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-level GPIO registration for the HD-RK3506-EVM.
 *
 * This is the board side of the R528-style GPIO model: the chip driver
 * (rk3506_gpio.c) exposes one ioexpander lower-half device per GPIO
 * bank; here we decide which physical pads are exported to user space
 * as /dev/gpioN character devices, by calling gpio_lower_half() once
 * per exported pin.
 *
 * Each map entry is:
 *
 *   minor    -> the N in /dev/gpioN
 *   bank     -> the GPIO bank (0..4) the pad lives on
 *   bank_pin -> the pin number inside that bank (0..31)
 *   pintype  -> one of enum gpio_pintype_e (GPIO_INPUT_PIN,
 *               GPIO_OUTPUT_PIN, GPIO_INTERRUPT_RISING_PIN, ...)
 *
 * Pads that are owned by an on-board driver (the NV3007 LCD control
 * lines on GPIO0, the GMAC0 PHY reset on GPIO4_A2, the UART/I2C/SPI
 * function pins, ...) must NOT be added here -- exporting them would
 * let user space fight the driver for the same pad.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/ioexpander/gpio.h>
#include <nuttx/ioexpander/ioexpander.h>

#include "drv/gpio/rk3506_gpio.h"

#ifdef CONFIG_IOEXPANDER

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hd_rk3506_gpio_map_s
{
  uint8_t              minor;     /* /dev/gpio{minor}               */
  uint8_t              bank;      /* GPIO bank 0..4                 */
  uint8_t              bank_pin;  /* pin inside the bank, 0..31     */
  enum gpio_pintype_e  pintype;   /* GPIO_*_PIN                     */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Exported-pin table.  Populate with the pads the application needs;
 * see the file header for the ownership rules.  Example (disabled):
 *
 *   { 0, 0, 21, GPIO_OUTPUT_PIN },  ->  /dev/gpio0 = GPIO0_C5
 *   { 1, 0, 22, GPIO_INPUT_PIN_PULLUP }, -> /dev/gpio1 = GPIO0_C6
 */

static const struct hd_rk3506_gpio_map_s g_hd_gpio_map[] =
{
  /* No board pins are exported by default.  Add entries here. */
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_gpio_initialize
 *
 * Description:
 *   Create the /dev/gpioN nodes listed in g_hd_gpio_map, one
 *   gpio_lower_half() per entry, on top of the per-bank ioexpander
 *   devices provided by the chip driver.
 *
 * Returned Value:
 *   OK on success (including an empty map); a negated errno if any
 *   bank fails to initialize or a node fails to register.
 *
 ****************************************************************************/

int hd_rk3506_gpio_initialize(void)
{
  int ret = OK;
  int i;

  for (i = 0; i < (int)(sizeof(g_hd_gpio_map) /
                       sizeof(g_hd_gpio_map[0])); i++)
    {
      FAR struct ioexpander_dev_s *ioe;

      ioe = rk3506_gpio_bank_initialize(g_hd_gpio_map[i].bank);
      if (ioe == NULL)
        {
          gpioerr("ERROR: bank %u initialize failed\n",
                  g_hd_gpio_map[i].bank);
          return -ENODEV;
        }

      ret = gpio_lower_half(ioe,
                            g_hd_gpio_map[i].bank_pin,
                            g_hd_gpio_map[i].pintype,
                            g_hd_gpio_map[i].minor);
      if (ret < 0)
        {
          gpioerr("ERROR: gpio_lower_half(bank %u pin %u) failed: %d\n",
                  g_hd_gpio_map[i].bank, g_hd_gpio_map[i].bank_pin, ret);
          return ret;
        }
    }

  return ret;
}

#endif /* CONFIG_IOEXPANDER */
