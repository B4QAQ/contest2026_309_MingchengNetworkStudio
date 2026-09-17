/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_saradc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SARADC (successive approximation ADC) driver for the RK3506G2, NuttX
 * adc lower-half integration (/dev/adc0).
 *
 * Hardware facts (Linux SDK):
 *  - SARADC v2 register map ("rockchip,rk3506-saradc" / "rockchip,
 *    rk3562-saradc"), base 0xFF4E8000 (u-boot/arch/arm/dts/rk3506.dtsi
 *    line 1077; IRQ = GIC SPI 57).
 *  - Register sequence ported from kernel-6.1/drivers/iio/adc/
 *    rockchip_saradc.c (rockchip_saradc_start_v2 / _read_v2) and the
 *    SDK HAL hal/lib/hal/src/hal_saradc.c (HAL_SARADC_Start):
 *      T_DAS_SOC  = 0xc
 *      T_PD_SOC   = 0x20
 *      END_INT_EN = END_INT(1) with HIWORD write-enable
 *      CONV_CON   = START(bit4) | SINGLE_MODE(bit5) | channel[3:0],
 *                   written with the same HIWORD write-enable
 *      EOC        -> END_INT_ST = 0x1 (plain write clears),
 *                    result = DATA[chn] @ 0x120 + 4*chn, 12-bit.
 *  - Kernel workaround kept verbatim: the controller is reset
 *    (SRST_P_SARADC pulse) before each conversion, "If read other chn
 *    at anytime, then chn1 will error" (rockchip_saradc.c:110-114).
 *  - Converter clock: CLK_SARADC composite (CLKSEL_CON(54), mux
 *    bits[5:4] {xin24m, clk_rc, clk_32k}, div bits[3:0]); the kernel
 *    requests 1 MHz.  With the 4-bit divider the closest achievable
 *    rate from xin24m (24 MHz) is 24/16 = 1.5 MHz (div = 15); the CCF
 *    lands on the same choice for a 1 MHz request.
 *  - Conversion is interrupt driven (END_INT, GIC SPI 57) exactly
 *    like the kernel; /dev/adc0 readers trigger a single-shot scan of
 *    the enabled channels via ioctl(ANIOC_TRIGGER) (the standard
 *    NuttX software-trigger flow used by apps/examples/adc with
 *    CONFIG_EXAMPLES_ADC_SWTRIG).
 *
 * Pins: SARADC_IN2 / SARADC_IN3 are dedicated analog pads on the
 * 40-pin expansion connector of the base board (1.8 V domain); they
 * are not routed through RM_IO and need no IOMUX setup.
 *
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>

#include "rk3506_cru.h"

#ifdef CONFIG_RK3506_SARADC

#ifndef putreg32
#  define putreg32(v, a) (*(FAR volatile uint32_t *)(a) = (v))
#endif
#ifndef getreg32
#  define getreg32(a)    (*(FAR volatile uint32_t *)(a))
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SARADC_BASE             0xFF4E8000u

/* Register offsets (rockchip_saradc.c SARADC2_* / rk3506.h) */

#define SARADC_CONV_CON         0x0000
#define SARADC_T_PD_SOC         0x0004
#define SARADC_T_DAS_SOC        0x000c
#define SARADC_END_INT_EN       0x0104
#define SARADC_END_INT_ST       0x0110
#define SARADC_DATA_BASE        0x0120

/* CONV_CON fields */

#define SARADC2_CONV_CHANNELS   0x0000000fu  /* GENMASK(3, 0) */
#define SARADC2_START           0x00000010u  /* BIT(4) */
#define SARADC2_SINGLE_MODE     0x00000020u  /* BIT(5) */
#define SARADC2_CONV_WE         (SARADC2_START | SARADC2_SINGLE_MODE | \
                                 SARADC2_CONV_CHANNELS)

#define SARADC_END_INT          0x00000001u

/* Data mask (rk3506.h SARADC_DATA0_DATA0_MASK) */

#define SARADC_DATA_MASK        0x00000fffu

/* CRU bits (clk-rk3506.c) */

#define CRU_ADDR                0xFF480000u
#define CRU_CLKGATE_CON(n)      (CRU_ADDR + (n) * 4u + 0x800u)
#define CRU_CLKSEL_CON(n)       (CRU_ADDR + (n) * 4u + 0x300u)
#define CRU_SOFTRST_CON(n)      (CRU_ADDR + (n) * 4u + 0xa00u)

#define CLKGATE19_PCLK_SARADC   (1u << 0)
#define CLKGATE19_CLK_SARADC    (1u << 1)

/* CLKSEL_CON(54): mux bits [5:4] (0 = xin24m), div bits [3:0].
 * div = 15 -> 24 MHz / 16 = 1.5 MHz converter clock.
 */

#define CLKSEL54_MUX_SHIFT      4
#define CLKSEL54_MUX_MASK       (0x3u << CLKSEL54_MUX_SHIFT)
#define CLKSEL54_DIV_SHIFT      0
#define CLKSEL54_DIV_MASK       (0xfu << CLKSEL54_DIV_SHIFT)
#define CLKSEL54_DIV_1P5MHZ     15u

/* SOFTRST_CON(19): SRST_P_SARADC = bit 0, SRST_SARADC = bit 1 */

#define SOFTRST19_SRST_P_SARADC (1u << 0)
#define SOFTRST19_SRST_SARADC   (1u << 1)

#define SARADC_IRQ              RK3506_IRQ_SARADC

#ifndef CONFIG_RK3506_SARADC_CHANMASK
#  define SARADC_CHANMASK       0x0c   /* default: IN2 + IN3 (header pins) */
#else
#  define SARADC_CHANMASK       CONFIG_RK3506_SARADC_CHANMASK
#endif

#define SARADC_MAX_CHANNELS     16

/* Conversion latency budget: at the 1.5 MHz converter clock the
 * T_PD_SOC (32) + T_DAS_SOC (12) + 12-bit conversion sequence is a
 * few dozen converter clocks; the END interrupt delivers the result.
 * This timeout only catches a dead controller (kernel: 100 ms).
 */

#define SARADC_TIMEOUT_MS       100

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_saradc_s
{
  struct adc_dev_s            *dev;       /* Upper half device */
  FAR const struct adc_callback_s *cb;    /* Upper half callbacks */
  uint32_t                     chanmask;  /* Enabled channels (scan order
                                           * = bit order, LSB first) */
  uint8_t                      cur;       /* Channel being converted */
  volatile bool                busy;      /* Scan in progress */
  volatile bool                rxint;     /* RX interrupt enabled */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  saradc_bind(FAR struct adc_dev_s *dev,
                        FAR const struct adc_callback_s *callback);
static void saradc_reset(FAR struct adc_dev_s *dev);
static int  saradc_setup(FAR struct adc_dev_s *dev);
static void saradc_shutdown(FAR struct adc_dev_s *dev);
static void saradc_rxint(FAR struct adc_dev_s *dev, bool enable);
static int  saradc_ioctl(FAR struct adc_dev_s *dev, int cmd,
                         unsigned long arg);
static int  saradc_interrupt(int irq, FAR void *context,
                             FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct adc_ops_s g_saradc_ops =
{
  .ao_bind     = saradc_bind,
  .ao_reset    = saradc_reset,
  .ao_setup    = saradc_setup,
  .ao_shutdown = saradc_shutdown,
  .ao_rxint    = saradc_rxint,
  .ao_ioctl    = saradc_ioctl,
};

static struct rk3506_saradc_s g_saradc;
static struct adc_dev_s *g_saradc_dev;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: saradc_pclk_reset
 *
 * Description:
 *   Pulse SRST_P_SARADC (assert 10-20 us, deassert), the kernel
 *   "reset controller" workaround performed before every conversion.
 *
 ****************************************************************************/

static void saradc_pclk_reset(void)
{
  /* Assert (set bit) then deassert (clear bit), HIWORD style */

  putreg32(SOFTRST19_SRST_P_SARADC << 16 | SOFTRST19_SRST_P_SARADC,
           CRU_SOFTRST_CON(19));
  up_udelay(10);
  putreg32(SOFTRST19_SRST_P_SARADC << 16, CRU_SOFTRST_CON(19));
}

/****************************************************************************
 * Name: saradc_start_channel
 *
 * Description:
 *   Start a single conversion of one channel (kernel start_v2).
 *   Interrupt context safe: register writes only.
 *
 ****************************************************************************/

static void saradc_start_channel(uint8_t ch)
{
  uint32_t val;

  /* Kernel workaround: reset the controller before converting a
   * channel other than the previous one.
   */

  saradc_pclk_reset();

  putreg32(0xc, SARADC_BASE + SARADC_T_DAS_SOC);
  putreg32(0x20, SARADC_BASE + SARADC_T_PD_SOC);

  /* Enable the end-of-conversion interrupt */

  putreg32((SARADC_END_INT << 16) | SARADC_END_INT,
           SARADC_BASE + SARADC_END_INT_EN);

  /* Select channel and start single conversion */

  val = (SARADC2_CONV_WE << 16) | SARADC2_START | SARADC2_SINGLE_MODE |
        (ch & SARADC2_CONV_CHANNELS);
  putreg32(val, SARADC_BASE + SARADC_CONV_CON);
}

/****************************************************************************
 * Name: saradc_ffs
 *
 * Description:
 *   Find first (least significant) set bit; n is at least 1.  Returns
 *   the 0-based bit index.
 *
 ****************************************************************************/

static uint8_t saradc_ffs(uint32_t n)
{
  return (uint8_t)(__builtin_ffs((int)n) - 1);
}

/****************************************************************************
 * Name: saradc_interrupt
 *
 * Description:
 *   END-of-conversion interrupt: clear the status, push the sample of
 *   the finished channel into the upper-half FIFO, then start the
 *   next channel of the scan.
 *
 ****************************************************************************/

static int saradc_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3506_saradc_s *priv = &g_saradc;
  uint32_t value;
  uint32_t mask;
  uint8_t  ch;
  uint8_t  next;

  /* Clear the end-of-conversion status (plain write of 0x1) */

  putreg32(0x1, SARADC_BASE + SARADC_END_INT_ST);

  ch    = priv->cur;
  value = getreg32(SARADC_BASE + SARADC_DATA_BASE + 4u * ch) &
          SARADC_DATA_MASK;

  /* Deliver to the upper half (FIFO) when RX interrupts are on */

  if (priv->rxint && priv->cb != NULL && priv->cb->au_receive != NULL)
    {
      priv->cb->au_receive(priv->dev, ch, (int32_t)value);
    }

  /* Start the next enabled channel, LSB-first scan order */

  mask   = priv->chanmask & ~((1u << (ch + 1)) - 1u);
  next   = (mask == 0u) ? 0u : saradc_ffs(mask);

  if (mask != 0u)
    {
      priv->cur = next;
      saradc_start_channel(next);
    }
  else
    {
      /* Scan finished */

      priv->busy = false;

      /* Disable the end interrupt until the next trigger */

      putreg32(SARADC_END_INT << 16, SARADC_BASE + SARADC_END_INT_EN);
    }

  return OK;
}

/****************************************************************************
 * Name: saradc_bind
 *
 * Description:
 *   Bind the upper-half callbacks (standard adc lower-half op).
 *
 ****************************************************************************/

static int saradc_bind(FAR struct adc_dev_s *dev,
                       FAR const struct adc_callback_s *callback)
{
  FAR struct rk3506_saradc_s *priv =
    (FAR struct rk3506_saradc_s *)dev->ad_priv;

  priv->cb  = callback;
  priv->dev = dev;
  return OK;
}

/****************************************************************************
 * Name: saradc_reset
 *
 * Description:
 *   Reset the ADC device (standard adc lower-half op): stop, disable
 *   IRQ, pulse the APB reset.
 *
 ****************************************************************************/

static void saradc_reset(FAR struct adc_dev_s *dev)
{
  FAR struct rk3506_saradc_s *priv =
    (FAR struct rk3506_saradc_s *)dev->ad_priv;

  up_disable_irq(SARADC_IRQ);
  putreg32(SARADC_END_INT << 16, SARADC_BASE + SARADC_END_INT_EN);
  putreg32(0x1, SARADC_BASE + SARADC_END_INT_ST);
  putreg32(0, SARADC_BASE + SARADC_CONV_CON);
  saradc_pclk_reset();

  priv->busy = false;
  priv->cur  = 0;
}

/****************************************************************************
 * Name: saradc_setup
 *
 * Description:
 *   Configure the ADC (standard adc lower-half op): attach and enable
 *   the end-of-conversion interrupt.
 *
 ****************************************************************************/

static int saradc_setup(FAR struct adc_dev_s *dev)
{
  FAR struct rk3506_saradc_s *priv =
    (FAR struct rk3506_saradc_s *)dev->ad_priv;
  int ret;

  ret = irq_attach(SARADC_IRQ, saradc_interrupt, NULL);
  if (ret < 0)
    {
      _err("SARADC: irq_attach failed: %d\n", ret);
      return ret;
    }

  up_enable_irq(SARADC_IRQ);

  priv->rxint = true;
  return OK;
}

/****************************************************************************
 * Name: saradc_shutdown
 *
 * Description:
 *   Disable the ADC (standard adc lower-half op).
 *
 ****************************************************************************/

static void saradc_shutdown(FAR struct adc_dev_s *dev)
{
  FAR struct rk3506_saradc_s *priv =
    (FAR struct rk3506_saradc_s *)dev->ad_priv;

  up_disable_irq(SARADC_IRQ);
  irq_detach(SARADC_IRQ);
  priv->rxint = false;
  priv->busy  = false;
}

/****************************************************************************
 * Name: saradc_rxint
 *
 * Description:
 *   Enable or disable RX interrupt delivery (standard adc lower-half
 *   op).
 *
 ****************************************************************************/

static void saradc_rxint(FAR struct adc_dev_s *dev, bool enable)
{
  FAR struct rk3506_saradc_s *priv =
    (FAR struct rk3506_saradc_s *)dev->ad_priv;

  priv->rxint = enable;
}

/****************************************************************************
 * Name: saradc_ioctl
 *
 * Description:
 *   ANIOC_TRIGGER: start a single-shot scan over the enabled channels
 *   (software-trigger flow of apps/examples/adc).
 *
 ****************************************************************************/

static int saradc_ioctl(FAR struct adc_dev_s *dev, int cmd,
                        unsigned long arg)
{
  FAR struct rk3506_saradc_s *priv =
    (FAR struct rk3506_saradc_s *)dev->ad_priv;
  irqstate_t flags;
  int ret = OK;

  switch (cmd)
    {
      case ANIOC_TRIGGER:
        {
          flags = enter_critical_section();

          if (!priv->busy)
            {
              priv->busy = true;
              priv->cur  = saradc_ffs(priv->chanmask);
              saradc_start_channel(priv->cur);
            }
          else
            {
              ret = -EBUSY;
            }

          leave_critical_section(flags);
        }
        break;

      default:
        _err("SARADC: unsupported ioctl %d\n", cmd);
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_saradc_initialize
 *
 * Description:
 *   Ungate the SARADC clocks, release its resets, and register
 *   /dev/adc0 (channels selected by CONFIG_RK3506_SARADC_CHANMASK,
 *   default IN2 + IN3 on the 40-pin header, 1.8 V domain).
 *
 * Returned Value:
 *   Zero on success; negated errno on failure.
 *
 ****************************************************************************/

int rk3506_saradc_initialize(void)
{
  FAR struct rk3506_saradc_s *priv = &g_saradc;
  uint32_t clksel;
  int ret;

  /* Clocks + resets (clk-rk3506.c): ungate PCLK_SARADC/CLK_SARADC,
   * converter clock 1.5 MHz, release SRST_P/SRST_SARADC.
   */

  rk3506_saradc_clock_init();

  /* Validate the channel mask: at least one channel must be enabled */

  memset(priv, 0, sizeof(*priv));
  priv->chanmask = SARADC_CHANMASK & ((1u << SARADC_MAX_CHANNELS) - 1u);
  priv->rxint    = false;

  if (priv->chanmask == 0)
    {
      _err("SARADC: empty channel mask\n");
      return -EINVAL;
    }

  /* Allocate the upper-half device and register /dev/adc0 */

  g_saradc_dev = kmm_zalloc(sizeof(struct adc_dev_s));
  if (g_saradc_dev == NULL)
    {
      _err("SARADC: adc_dev_s alloc failed\n");
      return -ENOMEM;
    }

  g_saradc_dev->ad_ops  = &g_saradc_ops;
  g_saradc_dev->ad_priv = priv;

  ret = adc_register("/dev/adc0", g_saradc_dev);
  if (ret < 0)
    {
      _err("SARADC: adc_register failed: %d\n", ret);
      kmm_free(g_saradc_dev);
      g_saradc_dev = NULL;
      return ret;
    }

  _info("SARADC /dev/adc0: chanmask=0x%" PRIx32 " (clk 1.5 MHz)\n",
        priv->chanmask);
  return OK;
}
#endif /* CONFIG_RK3506_SARADC */
