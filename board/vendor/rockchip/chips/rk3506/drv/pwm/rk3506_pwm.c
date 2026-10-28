/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_pwm.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PWM driver for the RK3506G2 (rk3576-style PWM IP), NuttX
 * pwm_lowerhalf_s integration.
 *
 * Hardware facts (Linux SDK):
 *  - The "pwm0_4ch" IP at 0xFF930000 has 4 independent channels, each
 *    with a complete register set at a 0x1000 stride (hal_pwm.h
 *    PWM_CHANNEL_OFFSET); the SDK board dts exposes every channel as
 *    its own node (pwm0_4ch_N @ 0xFF930000 + N*0x1000).
 *  - This driver fronts ONE channel block, defaulting to
 *    pwm0_4ch_2 @ 0xFF932000 - the channel wired to RM_IO12
 *    (GPIO0_B4 = J9-29, func 47 -> PWM0_CH2) on the HD-RK3506-EVM
 *    header.  (The SDK iot board also uses pwm0_4ch_2 for the LCD
 *    backlight, period 25000 ns; the HD-RK3506-EVM backlight is a
 *    plain GPIO instead, see the pin-routing note below.)
 *  - Time base: CLK_PWM0 <- clk_gpll_div_100m = 100 MHz (GPLL
 *    1.2 GHz; parent chain per kernel-6.1/drivers/clk/rockchip/
 *    clk-rk3506.c lines 188-193, 729-731), divider left at /1.
 *
 * Register sequence ported from SDK hal/lib/hal/src/hal_pwm.c
 * (HAL_PWM_SetConfig / HAL_PWM_Enable / HAL_PWM_Disable):
 *  PERIOD/DUTY  = ns * timebase_Hz / 1e9 (HAL: freqKhz*ns/(scaler*1e6)
 *               with scaler 0 -> /1e6 and freq = 100 MHz)
 *  CTRL         = continuous mode + polarity, HIWORD_UPDATE style
 *  ENABLE       = CTRL_UPDATE_EN then PWM_EN | PWM_CLK_EN
 *
 * Pin routing for RM_IO12 (GPIO0_B4 = J9-29, bank 0 in-bank pin 12),
 * per rk3506-pinctrl-rmio.dtsi rm_io12_pwm0_ch2 = <0 RK_PB4 47>:
 *  1. grf_pmu (0xFF910000) + 0x80 + 4*12 = routing function 47-15=32
 *  2. GPIO0_B4 iomux (ioc_pmu 0xFF950000 + 0x0C) nibble 0 = 7
 * This pad is J9-29 on the HD-RK3506-EVM, where it carries the NV3007
 * backlight.  That driver (hd_rk3506_nv3007.c) owns the pad as a plain
 * GPIO output, so CONFIG_RK3506_PWM is off on that board.
 *
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <debug.h>

#include <nuttx/timers/pwm.h>

#include "rk3506_cru.h"
#include "rk3506_pinctrl.h"
#include "rk3506_register.h"
#include "hardware/rk3506_memorymap.h"

#ifdef CONFIG_RK3506_PWM

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Channel register block offsets (hal_pwm.c / rk3506.h PWM_REG) */

#define PWM_REG_ENABLE          0x0004
#define PWM_REG_CLK_CTRL        0x0008
#define PWM_REG_CTRL            0x000c
#define PWM_REG_PERIOD          0x0010
#define PWM_REG_DUTY            0x0014

/* HIWORD_UPDATE: value in low half, write-enable mask in high half */

#define HIWORD_UPDATE(v, l, h) \
  (((uint32_t)(v) << (l)) | (((0xffffffffu << (l)) ^ \
   ((0xffffffffu << (h)) << 1)) << 16))

/* PWM_ENABLE (hal_pwm.c) */

#define PWM_CLK_EN              HIWORD_UPDATE(1, 0, 0)
#define PWM_EN                  HIWORD_UPDATE(1, 1, 1)
#define PWM_CTRL_UPDATE_EN      HIWORD_UPDATE(1, 2, 2)

/* PWM_CTRL (hal_pwm.c): mode 1 = continuous.  Polarity field value
 * 1 in bits [3:2] = duty positive / inactive negative.
 */

#define PWM_MODE_CONTINUOUS     HIWORD_UPDATE(1, 0, 1)
#define PWM_POLARITY_NORMAL     HIWORD_UPDATE(1, 2, 3)
#define PWM_POLARITY_INVERTED   HIWORD_UPDATE(2, 2, 3)

/* PWM0 channel 2 is routed out through the RM_IO matrix via the
 * unified rk3506_pinctrl_set_mux(); the pad is board-configurable
 * (CONFIG_RK3506_PWM_BANK/PIN/FUNC, default RM_IO12 = GPIO0_B4 =
 * J9-29 -> PWM0_CH2, dts function 47).  No local RM_IO/IOMUX register
 * pokes here.
 */

/* PWM0 channel 2 register block: RK3506_PWM0_4CH_2_ADDR from
 * hardware/rk3506_memorymap.h (single shared address table), and the
 * default frequency.
 */

#define PWM0_CH2_BASE           RK3506_PWM0_4CH_2_ADDR
/* 40 kHz, matches the SDK backlight mode */
#define PWM_DEFAULT_FREQUENCY   40000

/* PWM time base (CLK_PWM0 = 100 MHz, divider /1) */

#define PWM_INPUT_CLOCK_HZ      100000000u
#define PWM_TIMEBASE_PER_NS     (PWM_INPUT_CLOCK_HZ / 1000000u) /* 100 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_pwm_s
{
  struct pwm_lowerhalf_s lower;   /* pwm_lowerhalf_s (must be first) */
  uint32_t base;                  /* Channel register block */
  uint8_t  channel;               /* IP channel index (2) */
  bool     started;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int pwm_setup(FAR struct pwm_lowerhalf_s *dev);
static int pwm_shutdown(FAR struct pwm_lowerhalf_s *dev);
static int pwm_start(FAR struct pwm_lowerhalf_s *dev,
                     FAR const struct pwm_info_s *info);
static int pwm_stop(FAR struct pwm_lowerhalf_s *dev);
static int pwm_ioctl(FAR struct pwm_lowerhalf_s *dev, int cmd,
                     unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pwm_ops_s g_pwm_ops =
{
  .setup      = pwm_setup,
  .shutdown   = pwm_shutdown,
  .start      = pwm_start,
  .stop       = pwm_stop,
  .ioctl      = pwm_ioctl,
};

static struct rk3506_pwm_s g_pwm0ch2;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pwm_setup
 *
 * Description:
 *   pwm_lowerhalf_s setup callback: leave the channel stopped.
 *   Configuration happens at start time (pwm_start()).
 *
 ****************************************************************************/

static int pwm_setup(FAR struct pwm_lowerhalf_s *dev)
{
  FAR struct rk3506_pwm_s *priv = (FAR struct rk3506_pwm_s *)dev;

  _info("PWM0 ch%u setup (base 0x%08" PRIx32 ")\n",
        priv->channel, priv->base);

  putreg32(0, priv->base + PWM_REG_ENABLE);
  priv->started = false;
  return OK;
}

/****************************************************************************
 * Name: pwm_shutdown
 *
 * Description:
 *   pwm_lowerhalf_s shutdown callback: stop the output.
 *
 ****************************************************************************/

static int pwm_shutdown(FAR struct pwm_lowerhalf_s *dev)
{
  FAR struct rk3506_pwm_s *priv = (FAR struct rk3506_pwm_s *)dev;

  putreg32(0, priv->base + PWM_REG_ENABLE);
  priv->started = false;
  return OK;
}

/****************************************************************************
 * Name: pwm_start
 *
 * Description:
 *   Configure period/duty from info (NuttX duty: 16-bit fraction of
 *   65536) and start continuous mode, per HAL_PWM_SetConfig() +
 *   HAL_PWM_Enable(CONTINUOUS).
 *
 ****************************************************************************/

static int pwm_start(FAR struct pwm_lowerhalf_s *dev,
                     FAR const struct pwm_info_s *info)
{
  FAR struct rk3506_pwm_s *priv = (FAR struct rk3506_pwm_s *)dev;
  uint64_t period_ns;
  uint64_t duty_ns;
  uint32_t period;
  uint32_t duty;

  if (info->frequency == 0)
    {
      _err("PWM0 ch%u: frequency is 0\n", priv->channel);
      return -EINVAL;
    }

  /* Convert NuttX frequency + 16-bit duty into ns (period_ns =
   * 1e9/frequency, rounded).  Duty is a 0..65535 fraction of the
   * period.
   */

  period_ns = (1000000000ull + info->frequency / 2) / info->frequency;
  duty_ns   = (period_ns * info->duty) >> 16;

  /* Counter cycles at the fixed 100 MHz time base (NOT the requested
   * output frequency!): cycles = ns * 100 MHz / 1e9 = ns / 10
   * (HAL_PWM_SetConfig with pPWM->freq = 100 MHz, scaler = 0).
   */

  period = (uint32_t)((period_ns * PWM_INPUT_CLOCK_HZ) / 1000000000ull);
  duty   = (uint32_t)((duty_ns * PWM_INPUT_CLOCK_HZ) / 1000000000ull);

  if (period < 2)
    {
      _err("PWM0 ch%u: %lu Hz too fast for 100 MHz time base\n",
           priv->channel, (unsigned long)info->frequency);
      return -ERANGE;
    }

  if (duty > period)
    {
      duty = period;
    }

  _info("PWM0 ch%u: freq=%lu Hz, %lu/%lu ns -> cycles %lu/%lu\n",
        priv->channel, (unsigned long)info->frequency,
        (unsigned long)duty_ns, (unsigned long)period_ns,
        (unsigned long)duty, (unsigned long)period);

  /* HAL_PWM_SetConfig(): PERIOD/DUTY, polarity, CTRL update */

  putreg32(period, priv->base + PWM_REG_PERIOD);
  putreg32(duty, priv->base + PWM_REG_DUTY);
  putreg32(PWM_MODE_CONTINUOUS | PWM_POLARITY_NORMAL,
           priv->base + PWM_REG_CTRL);
  putreg32(PWM_CTRL_UPDATE_EN, priv->base + PWM_REG_ENABLE);

  /* HAL_PWM_Enable(CONTINUOUS): mode + PWM_EN | PWM_CLK_EN */

  putreg32(PWM_MODE_CONTINUOUS, priv->base + PWM_REG_CTRL);
  putreg32(PWM_EN | PWM_CLK_EN, priv->base + PWM_REG_ENABLE);

  priv->started = true;
  return OK;
}

/****************************************************************************
 * Name: pwm_stop
 *
 * Description:
 *   HAL_PWM_Disable(): clear ENABLE (PWM_EN | PWM_CLK_EN).
 *
 ****************************************************************************/

static int pwm_stop(FAR struct pwm_lowerhalf_s *dev)
{
  FAR struct rk3506_pwm_s *priv = (FAR struct rk3506_pwm_s *)dev;

  putreg32(0, priv->base + PWM_REG_ENABLE);
  priv->started = false;
  return OK;
}

/****************************************************************************
 * Name: pwm_ioctl
 *
 * Description:
 *   No chip-specific ioctls.
 *
 ****************************************************************************/

static int pwm_ioctl(FAR struct pwm_lowerhalf_s *dev, int cmd,
                     unsigned long arg)
{
  return -ENOTTY;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_pwm_initialize
 *
 * Description:
 *   Enable the PWM0 clocks, route RM_IO12 (GPIO0_B4 = J9-29) to PWM0_CH2,
 *   register /dev/pwm0 backed by the pwm0_4ch_2 channel block
 *   (0xFF932000).
 *
 * Returned Value:
 *   Zero on success; negated errno on failure.
 *
 ****************************************************************************/

int rk3506_pwm_initialize(void)
{
  FAR struct rk3506_pwm_s *priv = &g_pwm0ch2;
  static bool initialized;
  int ret = OK;

  if (initialized)
    {
      /* The pad/clock setup is idempotent and the device must be
       * registered only once.
       */

      return OK;
    }

  /* Route the pad to PWM0_CH2 through the unified pin-control.  The
   * pad is a board choice taken from CONFIG_RK3506_PWM_BANK/PIN/FUNC
   * (default rm_io12_pwm0_ch2 = <0 RK_PB4 47>, HD-RK3506-EVM J9-29).
   * rk3506_pinctrl_set_mux() sees the RM_IO function number (> 15),
   * programs the routing channel with (func - 15) and then forces the
   * local IOMUX nibble to the matrix path (7).
   */

  rk3506_pinctrl_set_mux((uint8_t)CONFIG_RK3506_PWM_BANK,
                         (uint8_t)CONFIG_RK3506_PWM_PIN,
                         (uint32_t)CONFIG_RK3506_PWM_FUNC);

  /* Clock gates for PWM0 (PCLK_PMU domain) live in rk3506_cru.c;
   * rk3506_pwm_clock_init() ungates PCLK_PWM0 and CLK_PWM0.
   */

  rk3506_pwm_clock_init();

  memset(priv, 0, sizeof(*priv));
  priv->lower.ops = &g_pwm_ops;
  priv->base      = PWM0_CH2_BASE;
  priv->channel   = 2;

  _info("PWM0 ch2 @ 0x%08" PRIx32 " on RM_IO12 (J9-29) -> /dev/pwm0\n",
        priv->base);

  ret = pwm_register("/dev/pwm0", &priv->lower);
  if (ret < 0)
    {
      return ret;
    }

  initialized = true;
  return OK;
}

/****************************************************************************
 * Name: rk3506_pwm_getlower
 *
 * Description:
 *   Return the PWM0 channel-2 lower half (RM_IO12 / GPIO0_B4 = J9-29) so an
 *   in-kernel consumer can start/stop it without going through the
 *   /dev/pwm0 character device.  (No in-tree consumer: the NV3007 backlight
 *   on this board is a plain GPIO, see hd_rk3506_nv3007.c.)
 *
 ****************************************************************************/

FAR struct pwm_lowerhalf_s *rk3506_pwm_getlower(void)
{
  return &g_pwm0ch2.lower;
}
#endif /* CONFIG_RK3506_PWM */
