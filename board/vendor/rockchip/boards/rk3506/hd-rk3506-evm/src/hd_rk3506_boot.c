/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_boot.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-specific early boot initialization for HD-RK3506-EVM.
 * Called from board_initialize() before any driver init.
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>

#include <nuttx/board.h>

#include "rk3506_cru.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_initialize
 *
 * Description:
 *   All RK3506 architectures must provide the following entry point.
 *   This entry point is called early in the initialization -- after all
 *   memory has been configured and mapped but before any devices have
 *   been initialized.
 *
 ****************************************************************************/

void board_initialize(void)
{
  /* Ungate GPIO0 clocks (PCLK + DBCLK) so that the RM_IO pads and the
   * GPIO0 controller are accessible.  This is normally done lazily by
   * rk3506_gpio_config(), but doing it here guarantees the clock is on
   * even if no GPIO driver is configured.
   */

  rk3506_gpio0_clock_init();

  /* Board-specific early initialization can be added here */
}
