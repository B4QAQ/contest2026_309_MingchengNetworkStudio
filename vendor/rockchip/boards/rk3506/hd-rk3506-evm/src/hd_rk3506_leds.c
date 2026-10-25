/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_leds.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * LED support for HD-RK3506-EVM.
 *
 * The green "run" LED is on GPIO1_B3 and lights at the LOW level
 * (SDK dts: heart_led = <&gpio1 RK_PB3 GPIO_ACTIVE_LOW>).  Hardware does
 * NOT route it to a PWM channel, so the Linux "heartbeat" behaviour is
 * reproduced in software: a low-priority kernel thread blinks the
 * Linux heartbeat pattern (two short flashes, long pause, ~1 Hz).
 *
 * The board also implements the standard NuttX userled lower half, so
 * the same LED is reachable as /dev/led0 via userled_lower_initialize().
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>

#include <debug.h>
#include <nuttx/clock.h>
#include <nuttx/kthread.h>
#include <nuttx/board.h>

#include "drv/gpio/rk3506_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Heartbeat LED: GPIO1_B3, active low (SDK dts: heart_led) */

#define LED_HEARTBEAT_GPIN  RK3506_GPIO_PIN(1, 11)   /* GPIO1_B3 */

/* Number of LEDs exported through the userled lower half */

#define HD_RK3506_NLEDS     1

/* Linux heartbeat-style pattern: two short flashes followed by a long
 * pause.  Each entry is {level, duration_ms}; the sequence repeats every
 * second (70+100+70+760 = 1000 ms).  level=true turns the LED on.
 */

struct led_phase_s
{
  bool     on;
  uint32_t ms;
};

static const struct led_phase_s g_heartbeat[] =
{
  { true,   70 },
  { false, 100 },
  { true,   70 },
  { false, 760 },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_userled_initialize
 ****************************************************************************/

#ifdef CONFIG_USERLED
uint32_t board_userled_initialize(void)
{
  int ret;

  /* Configure heartbeat LED as output, initially off (high = off for
   * active low).
   */

  ret = rk3506_gpio_config(LED_HEARTBEAT_GPIN, RK3506_GPIO_OUTPUT);
  if (ret < 0)
    {
      _err("LED: GPIO1_B3 config failed: %d\n", ret);
      return 0;
    }

  rk3506_gpio_write(LED_HEARTBEAT_GPIN, true);  /* High = LED off */

  return HD_RK3506_NLEDS;
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

      rk3506_gpio_write(LED_HEARTBEAT_GPIN, !ledon);
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

/****************************************************************************
 * Name: hd_rk3506_heartbeat_thread
 *
 * Description:
 *   Software heartbeat source.  NuttX has no device-tree equivalent of
 *   Linux's default-trigger="heartbeat", so this thread drives the run
 *   LED with the classic double-flash pattern.
 ****************************************************************************/

#ifdef CONFIG_HD_RK3506_LED_HEARTBEAT
static int hd_rk3506_heartbeat_thread(int argc, char *argv[])
{
  unsigned int phase;

  for (; ; )
    {
      for (phase = 0;
           phase < sizeof(g_heartbeat) / sizeof(g_heartbeat[0]);
           phase++)
        {
          board_userled(0, g_heartbeat[phase].on);
          nxsig_usleep((useconds_t)g_heartbeat[phase].ms * 1000);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: hd_rk3506_led_heartbeat_start
 *
 * Description:
 *   Start the software heartbeat thread.  Called once from board bringup.
 ****************************************************************************/

int hd_rk3506_led_heartbeat_start(void)
{
  int ret;

  ret = kthread_create("heartbeat", SCHED_PRIORITY_DEFAULT - 10, 1024,
                       hd_rk3506_heartbeat_thread, NULL);
  if (ret < 0)
    {
      _err("LED: heartbeat thread create failed: %d\n", ret);
    }

  return ret;
}
#endif /* CONFIG_HD_RK3506_LED_HEARTBEAT */
