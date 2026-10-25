/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_mmc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-level bring-up for the TF/SD socket on the HD-RK3506-EVM:
 * pin mux, SD rail power switch, card-detect GPIO and (optionally) a
 * low-rate card-detect poll feeding the MMCSD hotplug callback.
 *
 * Wiring reference (Linux SDK, THIS board - HD-RK3506-EVM):
 *  - sdmmc_clk/cmd/d0..d3 = GPIO3_A0..A5, function 1
 *      u-boot/arch/arm/dts/rk3506-pinctrl.dtsi:1046-1068
 *  - SD 3.3 V rail         = always-on vcc3v3_stb, NO GPIO power switch
 *      kernel-6.1/arch/arm/boot/dts/vanxoak-hd-rk3506-evm-v1.dtsi:
 *      vmmc-supply/vqmmc-supply = <&vcc3v3_stb>
 *  - card detect            = GPIO3_A6, active low, internal pull-up
 *      vanxoak-hd-rk3506-evm-v1.dtsi: cd_gpio =
 *      <3 RK_PA6 RK_FUNC_GPIO &pcfg_pull_up>
 *
 * (The rk3506g-iotest-v10.dts values - PWREN on A6, CD on A7 - belong to
 * a different carrier and are WRONG for this board: A7 is RMII1_RXD1.)
 *
 * All four SDMMC pads are plain IOMUX nibbles (function value 1 <= 15), so
 * rk3506_pinctrl_set_mux() applies them directly with no RM_IO routing.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/clock.h>
#include <nuttx/kthread.h>

#include <debug.h>

#include "drv/gpio/rk3506_gpio.h"
#include "rk3506_mmc.h"
#include "rk3506_pinctrl.h"

#ifdef CONFIG_RK3506_MMC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MMC_PIN_BANK              3
#define MMC_PIN_CLK               0    /* GPIO3_A0, func 1 */
#define MMC_PIN_CMD               1    /* GPIO3_A1, func 1 */
#define MMC_PIN_D0                2    /* GPIO3_A2, func 1 */
#define MMC_PIN_D3                5    /* GPIO3_A5, func 1 */
#define MMC_MUX_FUNC_SDMMC        1    /* rk3506-pinctrl.dtsi: <3 RK_PAx 1> */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void hd_rk3506_mmc_pins(void)
{
  int pin;

  /* sdmmc_clk, sdmmc_cmd, sdmmc_d0..d3 = GPIO3_A0..A5, function 1 */

  for (pin = MMC_PIN_CLK; pin <= MMC_PIN_D3; pin++)
    {
      if (rk3506_pinctrl_set_mux(MMC_PIN_BANK, pin, MMC_MUX_FUNC_SDMMC) < 0)
        {
          _err("MMC: IOMUX failed for GPIO3_A%d\n", pin);
        }

      /* The reference dts uses pull_none on every SDMMC pad; the card
       * side provides the pull-ups required by the SD spec.
       */

      rk3506_pinctrl_set_pull(MMC_PIN_BANK, pin, RK3506_PULL_DISABLE);
    }

#if !defined(CONFIG_RK3506_MMC_CD_GPIO)
  /* Controller-side detect fallback: SDMMC_DET mux.  NOTE: on the
   * HD-RK3506-EVM this pad is not the socket switch (CD is GPIO3_A6),
   * so this path is not used on this board.
   */

  if (rk3506_pinctrl_set_mux(0, 8, 1) < 0)
    {
      _err("MMC: SDMMC_DET IOMUX (GPIO0_B0) failed\n");
    }

  rk3506_pinctrl_set_pull(0, 8, RK3506_PULL_UP);
#endif
}

#ifdef CONFIG_RK3506_MMC_HOTPLUG
static int hd_rk3506_mmc_poll(int argc, char *argv[])
{
  for (; ; )
    {
      rk3506_mmc_carddetect();
      nxsig_usleep(CONFIG_RK3506_MMC_POLL_MS * 1000);
    }

  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_mmc_initialize
 *
 * Description:
 *   Board entry point called from hd_rk3506_bringup().
 *
 ****************************************************************************/

int hd_rk3506_mmc_initialize(void)
{
  int ret;

  /* 1. Mux the SDMMC pads before touching the controller */

  hd_rk3506_mmc_pins();

  /* 2. Optional socket power switch.  The HD-RK3506-EVM SD rail is the
   *    always-on vcc3v3_stb regulator (no GPIO gate), so PWREN is off in
   *    the board defconfig.  Kept generic for carriers that do gate it.
   */

#ifdef CONFIG_RK3506_MMC_PWREN
  ret = rk3506_gpio_config(RK3506_GPIO_PIN(CONFIG_RK3506_MMC_PWREN_BANK,
                                           CONFIG_RK3506_MMC_PWREN_PIN),
                           RK3506_GPIO_OUTPUT |
#ifdef CONFIG_RK3506_MMC_PWREN_ACTIVELOW
                           RK3506_GPIO_VALUE_LOW |
#else
                           RK3506_GPIO_VALUE_HIGH |
#endif
                           RK3506_GPIO_PULL_DISABLE);
  if (ret < 0)
    {
      _err("MMC: SD power switch GPIO config failed: %d\n", ret);
    }

  /* SD physical layer spec: >=1 ms supply ramp before identification. */

  nxsig_usleep(5000);
#endif

  /* 3. Card detect input (the socket switch is active low, pulled up) */

#ifdef CONFIG_RK3506_MMC_CD_GPIO
  ret = rk3506_gpio_config(RK3506_GPIO_PIN(CONFIG_RK3506_MMC_CD_BANK,
                                           CONFIG_RK3506_MMC_CD_PIN),
                           RK3506_GPIO_INPUT | RK3506_GPIO_PULL_UP);
  if (ret < 0)
    {
      _err("MMC: card detect GPIO config failed: %d\n", ret);
    }
#endif

  /* 4. Register the slot */

  ret = rk3506_mmc_initialize(0);
  if (ret < 0)
    {
      _err("MMC: rk3506_mmc_initialize failed: %d\n", ret);
      return ret;
    }

  /* 5. Hotplug poll: no CD interrupt is wired on this board */

#ifdef CONFIG_RK3506_MMC_HOTPLUG
  ret = kthread_create("mmc_poll", SCHED_PRIORITY_DEFAULT, 2048,
                       hd_rk3506_mmc_poll, NULL);
  if (ret < 0)
    {
      _warn("MMC: card detect poll thread failed: %d\n", ret);
    }
#endif

  return OK;
}

#endif /* CONFIG_RK3506_MMC */
