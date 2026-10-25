/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_btndbg.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NSH "btn" diagnostic: print the raw pad level of the two J9 buttons
 * (NEXT = J9-18/GPIO0_C2/pin18, OK = J9-20/GPIO0_C4/pin20).
 * Level 1 = released (internal pull-up), 0 = pressed (shorted to GND).
 * Press and hold a button, run `btn`, and the level must read 0; if it
 * stays 1 the wire/pad path is open.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>

#include "drv/gpio/rk3506_gpio.h"

#if defined(CONFIG_HD_RK3506_LV_BUTTONS)

int btn_main(int argc, char *argv[])
{
  static const struct
    {
      const char *name;
      uint8_t     gpin;
    } pins[2] =
    {
      { "NEXT J9-18 GPIO0_C2", RK3506_GPIO_PIN(0, 18) },
      { "OK   J9-20 GPIO0_C4", RK3506_GPIO_PIN(0, 20) },
    };

  int i;

  (void)argc;
  (void)argv;

  for (i = 0; i < 2; i++)
    {
      bool level = false;
      int ret = rk3506_gpio_read(pins[i].gpin, &level);

      if (ret < 0)
        {
          printf("%s: read failed (%d)\n", pins[i].name, ret);
        }
      else
        {
          printf("%s: level=%d (%s)\n", pins[i].name, (int)level,
                 level ? "released" : "PRESSED");
        }
    }

  return 0;
}

#endif /* CONFIG_HD_RK3506_LV_BUTTONS */
