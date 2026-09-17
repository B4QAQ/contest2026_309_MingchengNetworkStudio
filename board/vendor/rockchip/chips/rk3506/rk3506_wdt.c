/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_wdt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watchdog timer driver for the RK3506G2 Synopsys DesignWare watchdog
 * (dts compatible "snps,dw-wdt"), NuttX watchdog lower-half integration.
 *
 * Hardware knowledge base (Linux SDK, authoritative):
 *  - u-boot/arch/arm/dts/rk3506.dtsi: wdt0 @0xFF260000 (GIC SPI 107),
 *    wdt1 @0xFF268000 (GIC SPI 108); clocks TCLK_WDTx ("tclk",
 *    watchdog counter clock, from xin24m_gate = 24 MHz) and PCLK_WDTx
 *    ("pclk", register access, pclk_bus_root).
 *  - hal/lib/hal/src/hal_wdt.c: register sequence, CRR kick value 0x76,
 *    TOP table semantics (cycles = 2^(16+top), WDT_MAX_TOP 15).
 *  - kernel-6.1/drivers/watchdog/dw_wdt.c: TORR = top | top << 4
 *    (TOPINIT field, WDOG_TIMEOUT_RANGE_TOPINIT_SHIFT), live timeout
 *    update needs a kick, RESP_MODE 0 = reset / 1 = pre-timeout IRQ
 *    then reset on the second timeout.
 *
 * Register map (struct WDT_REG, rk3506.h):
 *   CR   0x00  WDT_EN bit0, RESP_MODE bit1, RST_PULSE_LENGTH bits[4:2]
 *   TORR 0x04  TOP[3:0] + TOPINIT[7:4]
 *   CCVR 0x08  current counter value (counts down)
 *   CRR  0x0c  counter restart (kick): write 0x76
 *   STAT 0x10  bit0: counter enabled
 *   EOI  0x14  read to clear the pre-timeout interrupt
 *
 * Timeout range with tclk = 24 MHz: top 0 = 2^16/24M ~= 2.73 ms,
 * top 15 = 2^31/24M ~= 89.5 s.
 *
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/kmalloc.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/spinlock.h>
#include <nuttx/timers/watchdog.h>

#include <debug.h>

#include "rk3506_cru.h"

#ifdef CONFIG_RK3506_WDT

#ifndef putreg32
#  define putreg32(v, a) (*(FAR volatile uint32_t *)(a) = (v))
#endif
#ifndef getreg32
#  define getreg32(a)    (*(FAR volatile uint32_t *)(a))
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WDT0_BASE               0xff260000u
#define WDT1_BASE               0xff268000u

#define WDT_IRQ0                RK3506_IRQ_WDT0
#define WDT_IRQ1                RK3506_IRQ_WDT1

/* Register offsets */

#define WDT_CR                  0x000
#define WDT_TORR                0x004
#define WDT_CCVR                0x008
#define WDT_CRR                 0x00c
#define WDT_STAT                0x010
#define WDT_EOI                 0x014

/* CR bits */

#define WDT_CR_EN               (1u << 0)
#define WDT_CR_RESP_MODE        (1u << 1)   /* 0=reset, 1=pre-timeout IRQ */

/* Kick / restart value (hal_wdt.c WDOG_COUNTER_RESTART_KICK_VALUE) */

#define WDT_KICK_VALUE          0x76

/* Counter clock: TCLK_WDTx = xin24m_gate = 24 MHz (clk-rk3506.c) */

#define WDT_TCLK_HZ             24000000u

/* TOP limits (DW_WDT_NUM_TOPS = 16, WDT_MAX_TOP = 15) */

#define WDT_MAX_TOP             15u
#define WDT_MIN_CYCLES          (1UL << 16)         /* top 0 */
#define WDT_MAX_CYCLES          (1UL << 31)         /* top 15 */

/* Max representable timeout in ms:
 * 2^31 cycles / 24 MHz * 1000 ~= 89478 ms
 */

#define WDT_MAXTIMEOUT_MS       (WDT_MAX_CYCLES / (WDT_TCLK_HZ / 1000))

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure provides the private representation of the "lower-half"
 * driver state structure.  This structure must be cast-compatible with the
 * well-known watchdog_lowerhalf_s structure.
 */

struct rk3506_wdt_lowerhalf_s
{
  FAR const struct watchdog_ops_s *ops; /* Lower half operations */
  uint32_t base;                        /* Register base address */
  uint32_t irq;                         /* Pre-timeout interrupt */
  uint32_t timeout;                     /* Current timeout (ms) */
  uint8_t  top;                         /* Current TORR TOP field */
  bool     started;                     /* Timer running */
  xcpt_t   handler;                     /* Pre-timeout user handler */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int wdt_start(FAR struct watchdog_lowerhalf_s *lower);
static int wdt_stop(FAR struct watchdog_lowerhalf_s *lower);
static int wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower);
static int wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                         FAR struct watchdog_status_s *status);
static int wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                          uint32_t timeout);
static xcpt_t wdt_capture(FAR struct watchdog_lowerhalf_s *lower,
                          xcpt_t handler);
static int wdt_interrupt(int irq, FAR void *context, FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* "Lower half" driver methods */

static const struct watchdog_ops_s g_wdtops =
{
  .start      = wdt_start,
  .stop       = wdt_stop,
  .keepalive  = wdt_keepalive,
  .getstatus  = wdt_getstatus,
  .settimeout = wdt_settimeout,
  .capture    = wdt_capture,
};

static struct rk3506_wdt_lowerhalf_s g_wdtdev[2];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wdt_find_top
 *
 * Description:
 *   Find the smallest TOP whose cycle count covers the requested
 *   timeout in milliseconds (dw_wdt_find_best_top semantics: TOP with
 *   timeout >= requested, maximum TOP if none reaches it).
 *
 *   cycles = 2^(16+top); timeout_ms * (tclk / 1000) <= cycles
 *   (the ms*24000 product fits 32 bit because timeout is range-checked
 *   to WDT_MAXTIMEOUT_MS first).
 *
 ****************************************************************************/

static uint8_t wdt_find_top(uint32_t timeout_ms)
{
  uint32_t cycles;
  uint8_t top;

  for (top = 0; top <= WDT_MAX_TOP; top++)
    {
      cycles = WDT_MIN_CYCLES << top;
      if (cycles >= timeout_ms * (WDT_TCLK_HZ / 1000))
        {
          return top;
        }
    }

  return WDT_MAX_TOP;
}

/****************************************************************************
 * Name: wdt_settop
 *
 * Description:
 *   Program TORR (TOP + TOPINIT) and kick the counter so a running
 *   watchdog picks the value up (kernel dw_wdt_set_timeout tail).
 *
 ****************************************************************************/

static void wdt_settop(FAR struct rk3506_wdt_lowerhalf_s *priv,
                       uint8_t top)
{
  priv->top = top;
  putreg32(top | (top << 4), priv->base + WDT_TORR);
  putreg32(WDT_KICK_VALUE, priv->base + WDT_CRR);
}

/****************************************************************************
 * Name: wdt_apply_mode
 *
 * Description:
 *   Apply the response mode: with a capture handler the first timeout
 *   raises the pre-timeout interrupt (INDIRECT_SYSTEM_RESET), without
 *   one the controller resets the system directly.
 *
 ****************************************************************************/

static void wdt_apply_mode(FAR struct rk3506_wdt_lowerhalf_s *priv)
{
  uint32_t cr = getreg32(priv->base + WDT_CR);

  if (priv->handler != NULL)
    {
      cr |= WDT_CR_RESP_MODE;
    }
  else
    {
      cr &= ~WDT_CR_RESP_MODE;
    }

  putreg32(cr, priv->base + WDT_CR);
}

/****************************************************************************
 * Name: wdt_start
 *
 * Description:
 *   Start the watchdog timer, resetting the counter to the current
 *   timeout.
 *
 ****************************************************************************/

static int wdt_start(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3506_wdt_lowerhalf_s *priv =
    (FAR struct rk3506_wdt_lowerhalf_s *)lower;
  uint32_t cr;
  int ret;

  if (!priv->started)
    {
      /* Clocks: ungate PCLK + TCLK (24 MHz), release resets */

      rk3506_wdt_clock_init(priv->base == WDT0_BASE ? 0 : 1);

      /* Program the timeout and apply the response mode */

      wdt_settop(priv, wdt_find_top(priv->timeout));
      wdt_apply_mode(priv);

      if (priv->handler != NULL)
        {
          ret = irq_attach(priv->irq, wdt_interrupt, priv);
          if (ret < 0)
            {
              return ret;
            }

          up_enable_irq(priv->irq);
        }

      cr = getreg32(priv->base + WDT_CR);
      putreg32(cr | WDT_CR_EN, priv->base + WDT_CR);
      priv->started = true;
    }

  return OK;
}

/****************************************************************************
 * Name: wdt_stop
 *
 * Description:
 *   Stop the watchdog timer.
 *
 ****************************************************************************/

static int wdt_stop(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3506_wdt_lowerhalf_s *priv =
    (FAR struct rk3506_wdt_lowerhalf_s *)lower;

  if (priv->started)
    {
      uint32_t cr = getreg32(priv->base + WDT_CR);

      putreg32(cr & ~WDT_CR_EN, priv->base + WDT_CR);

      if (priv->handler != NULL)
        {
          up_disable_irq(priv->irq);
        }

      priv->started = false;
    }

  return OK;
}

/****************************************************************************
 * Name: wdt_keepalive
 *
 * Description:
 *   Reset the watchdog timer to the current timeout value ("pinging"
 *   or "petting" the watchdog).
 *
 ****************************************************************************/

static int wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3506_wdt_lowerhalf_s *priv =
    (FAR struct rk3506_wdt_lowerhalf_s *)lower;

  putreg32(WDT_KICK_VALUE, priv->base + WDT_CRR);
  return OK;
}

/****************************************************************************
 * Name: wdt_getstatus
 *
 * Description:
 *   Get the current watchdog timer status.
 *
 ****************************************************************************/

static int wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                         FAR struct watchdog_status_s *status)
{
  FAR struct rk3506_wdt_lowerhalf_s *priv =
    (FAR struct rk3506_wdt_lowerhalf_s *)lower;

  memset(status, 0, sizeof(*status));

  status->timeout = priv->timeout;
  status->flags   = priv->started ? WDFLAGS_ACTIVE : 0;
  status->flags  |= priv->handler == NULL ? WDFLAGS_RESET :
                                            WDFLAGS_CAPTURE;

  if (priv->started)
    {
      /* CCVR counts down at TCLK (24 MHz); max top fits 32 bit
       * (2^31 cycles), so the division is safe in 32 bit math.
       */

      uint32_t ccvr = getreg32(priv->base + WDT_CCVR);
      status->timeleft = ccvr / (WDT_TCLK_HZ / 1000);
    }

  return OK;
}

/****************************************************************************
 * Name: wdt_settimeout
 *
 * Description:
 *   Set a new timeout value (and reset the watchdog timer to it if it
 *   is already started).
 *
 ****************************************************************************/

static int wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                          uint32_t timeout)
{
  FAR struct rk3506_wdt_lowerhalf_s *priv =
    (FAR struct rk3506_wdt_lowerhalf_s *)lower;
  uint8_t top;

  /* Can this timeout be represented by one of the 16 TOPs? */

  if (timeout < 1 || timeout > WDT_MAXTIMEOUT_MS)
    {
      wderr("ERROR: Cannot represent timeout=%" PRIu32 " ms "
            "(range 1..%lu)\n", timeout, (unsigned long)WDT_MAXTIMEOUT_MS);
      return -ERANGE;
    }

  top = wdt_find_top(timeout);
  priv->timeout = timeout;

  if (priv->started)
    {
      /* Live update: program TORR and kick the counter */

      wdt_settop(priv, top);
    }
  else
    {
      priv->top = top;
    }

  wdinfo("timeout=%" PRIu32 " ms -> top=%u (%lu ms)\n",
         timeout, top,
         (unsigned long)((WDT_MIN_CYCLES << top) / (WDT_TCLK_HZ / 1000)));
  return OK;
}

/****************************************************************************
 * Name: wdt_capture
 *
 * Description:
 *   Don't reset on watchdog timer timeout; instead call this user
 *   provided timeout handler.  Providing handler==NULL will restore
 *   the reset behavior.  The DW watchdog implements this as its
 *   two-stage IRQ mode: the first timeout raises the interrupt, the
 *   second performs the system reset.
 *
 ****************************************************************************/

static xcpt_t wdt_capture(FAR struct watchdog_lowerhalf_s *lower,
                          xcpt_t handler)
{
  FAR struct rk3506_wdt_lowerhalf_s *priv =
    (FAR struct rk3506_wdt_lowerhalf_s *)lower;
  xcpt_t oldhandler = priv->handler;
  irqstate_t flags;

  flags = enter_critical_section();
  priv->handler = handler;

  if (priv->started)
    {
      wdt_apply_mode(priv);

      if (handler != NULL)
        {
          irq_attach(priv->irq, wdt_interrupt, priv);
          up_enable_irq(priv->irq);
        }
      else
        {
          up_disable_irq(priv->irq);
        }
    }

  leave_critical_section(flags);
  return oldhandler;
}

/****************************************************************************
 * Name: wdt_interrupt
 *
 * Description:
 *   Pre-timeout interrupt (RESP_MODE = 1): read EOI to clear the
 *   interrupt, then deliver to the user handler.  If the condition
 *   is not handled, the second timeout resets the system.
 *
 ****************************************************************************/

static int wdt_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3506_wdt_lowerhalf_s *priv =
    (FAR struct rk3506_wdt_lowerhalf_s *)arg;

  /* EOI read clears the pending interrupt */

  putreg32(getreg32(priv->base + WDT_EOI), priv->base + WDT_EOI);

  if (priv->handler != NULL)
    {
      priv->handler(irq, context, NULL);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_wdt_initialize
 *
 * Description:
 *   Initialize the DW watchdog timers and register them as
 *   /dev/watchdog0 (WDT0 @0xFF260000) and /dev/watchdog1 (WDT1
 *   @0xFF268000).  The initial state of each watchdog is disabled.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is
 *   returned on any failure.
 *
 ****************************************************************************/

int rk3506_wdt_initialize(void)
{
  FAR struct rk3506_wdt_lowerhalf_s *priv;
  FAR void *handle;
  int ret;
  int i;

  for (i = 0; i < 2; i++)
    {
      priv = &g_wdtdev[i];

      memset(priv, 0, sizeof(*priv));
      priv->ops  = &g_wdtops;

      if (i == 0)
        {
          priv->base = WDT0_BASE;
          priv->irq  = WDT_IRQ0;
        }
      else
        {
          priv->base = WDT1_BASE;
          priv->irq  = WDT_IRQ1;
        }

      /* Register the watchdog driver as /dev/watchdogN */

      handle = watchdog_register(i == 0 ? "/dev/watchdog0" :
                                          "/dev/watchdog1",
                                 (FAR struct watchdog_lowerhalf_s *)priv);
      if (handle == NULL)
        {
          wderr("ERROR: Failed to register /dev/watchdog%d\n", i);
          return -ENODEV;
        }

      ret = OK;
    }

  return ret;
}
#endif /* CONFIG_RK3506_WDT */
