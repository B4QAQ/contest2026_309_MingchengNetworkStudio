/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_leds.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * LED support for HD-RK3506-EVM.
 * Currently a stub; no LEDs are wired on this board.
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_userled_initialize
 ****************************************************************************/

#ifdef CONFIG_USERLED
int board_userled_initialize(void)
{
  /* No LEDs wired on this board */

  return 0;
}
#endif

/****************************************************************************
 * Name: board_userled
 ****************************************************************************/

#ifdef CONFIG_USERLED
void board_userled(int led, bool ledon)
{
  /* No LEDs wired on this board */
}
#endif

/****************************************************************************
 * Name: board_userled_all
 ****************************************************************************/

#ifdef CONFIG_USERLED
void board_userled_all(uint32_t ledset)
{
  /* No LEDs wired on this board */
}
#endif
