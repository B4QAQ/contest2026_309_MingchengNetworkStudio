/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_leds.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * LED support for HD-RK3506-EVM.
 * Heartbeat LED on GPIO1_B3 (SDK dts: heart_led, active low).
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>

#include "drv/gpio/rk3506_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Heartbeat LED: GPIO1_B3, active low (SDK dts: heart_led) */

#define LED_HEARTBEAT_BANK  1
#define LED_HEARTBEAT_PIN   11    /* GPIO1_B3 */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_userled_initialize
 ****************************************************************************/

#ifdef CONFIG_USERLED
int board_userled_initialize(void)
{
  /* Configure heartbeat LED as output, initially off (high = off for
   * active low)
   */

  rk3506_gpio_config(LED_HEARTBEAT_BANK, LED_HEARTBEAT_PIN,
                     RK3506_GPIO_OUTPUT);
  rk3506_gpio_write(LED_HEARTBEAT_BANK, LED_HEARTBEAT_PIN,
                    RK3506_GPIO_VALUE_HIGH);

  return 0;
}
#endif

/****************************************************************************
 * Name: board_userled
 ****************************************************************************/

#ifdef CONFIG_USERLED
void board_userled(int led, bool ledon)
{
  if (led == 0)
    {
      /* Heartbeat LED (active low) */

      rk3506_gpio_write(LED_HEARTBEAT_BANK, LED_HEARTBEAT_PIN,
                        ledon ? RK3506_GPIO_VALUE_LOW :
                                RK3506_GPIO_VALUE_HIGH);
    }
}
#endif

/****************************************************************************
 * Name: board_userled_all
 ****************************************************************************/

#ifdef CONFIG_USERLED
void board_userled_all(uint32_t ledset)
{
  board_userled(0, (ledset & 1) != 0);
}
#endif
