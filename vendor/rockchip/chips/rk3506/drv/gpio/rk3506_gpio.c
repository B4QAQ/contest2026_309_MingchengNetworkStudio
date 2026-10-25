/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_gpio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO controller driver for the Rockchip RK3506G2 (GPIO v2.2 IP).
 *
 * Register model (all offsets/semantics taken from the Linux
 * gpio-rockchip.c v2 driver, gpio-rockchip.c:92-107 / 116-190, and the
 * CMSIS struct GPIO_REG, rk3506.h:230-275):
 *
 *   - Data (DR), direction (DDR), interrupt enable/mask/type/polarity/
 *     bothedge and the EOI registers are all split into a low and a
 *     high 16-bit half-word register pair (pins 0..15 in _L, 16..31 in
 *     _H).
 *   - Every write is a HIWORD update: the high half-word carries the
 *     per-bit write-enable mask, the low half-word the data, so a single
 *     pin is updated with one store and no read-modify-write:
 *
 *       reg = (BIT(pin % 16) << 16) | (value << (pin % 16))
 *
 *   - Reading the pad level uses EXT_PORT (0x70), a single 32-bit
 *     register for all 32 pins (no hiword split).
 *   - Edge interrupts are acknowledged by writing 1 to the matching
 *     PORT_EOI bit (write-1-to-clear); INT_STATUS (0x50) reports the
 *     masked, pending interrupt bits.
 *
 * Interrupt routing: each bank wires one combined level-high interrupt
 * to the GIC (RK3506_IRQ_GPIO{0..4}_0, see include/irq.h).  The ISR
 * demultiplexes INT_STATUS and dispatches the per-pin callbacks
 * registered through rk3506_gpio_irq_attach().
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_pinctrl.h"
#include "rk3506_cru.h"
#include "drv/gpio/rk3506_gpio.h"
#include "rk3506_irq.h"
#include "rk3506_register.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO v2.2 register offsets (from the bank base) */

#define GPIO_SWPORT_DR_L       0x00u
#define GPIO_SWPORT_DR_H       0x04u
#define GPIO_SWPORT_DDR_L      0x08u
#define GPIO_SWPORT_DDR_H      0x0cu
#define GPIO_INT_EN_L          0x10u
#define GPIO_INT_EN_H          0x14u
#define GPIO_INT_MASK_L        0x18u
#define GPIO_INT_MASK_H        0x1cu
#define GPIO_INT_TYPE_L        0x20u
#define GPIO_INT_TYPE_H        0x24u
#define GPIO_INT_POLARITY_L    0x28u
#define GPIO_INT_POLARITY_H    0x2cu
#define GPIO_INT_BOTHEDGE_L    0x30u
#define GPIO_INT_BOTHEDGE_H    0x34u
#define GPIO_DEBOUNCE_L        0x38u
#define GPIO_DEBOUNCE_H        0x3cu
#define GPIO_DBCLK_DIV_EN_L    0x40u
#define GPIO_DBCLK_DIV_EN_H    0x44u
#define GPIO_DBCLK_DIV_CON     0x48u
#define GPIO_INT_STATUS        0x50u
#define GPIO_INT_RAWSTATUS     0x58u
#define GPIO_PORT_EOI_L        0x60u
#define GPIO_PORT_EOI_H        0x64u
#define GPIO_EXT_PORT          0x70u
#define GPIO_VER_ID            0x78u

/* Interrupt trigger encodings (INT_TYPE / INT_POLARITY / BOTHEDGE),
 * matching gpio-rockchip.c rockchip_gpio_set_type():493-576.
 *   INT_TYPE     : 0 = level,      1 = edge
 *   INT_POLARITY : 0 = low/falling 1 = high/rising
 *   INT_BOTHEDGE : 1 = both edges (overrides polarity)
 */

#define RK3506_TRIG_LEVEL_HIGH   0u
#define RK3506_TRIG_LEVEL_LOW    1u
#define RK3506_TRIG_EDGE_RISING  2u
#define RK3506_TRIG_EDGE_FALLING 3u
#define RK3506_TRIG_EDGE_BOTH    4u

/* Number of GPIO banks */

#define RK3506_GPIO_NBANKS  5u
#define RK3506_BANK_NPINS   32u

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Per-bank runtime state */

struct rk3506_gpio_bank_s
{
  uintptr_t               base;     /* bank register base address      */
  uint8_t                 irq;      /* combined GIC IRQ number         */
  bool                    clk_on;   /* clocks have been ungated        */
#ifdef CONFIG_IOEXPANDER
  struct ioexpander_dev_s ioe;      /* ioexpander lower-half device    */
#endif
#ifdef CONFIG_IOEXPANDER_INT_ENABLE
  ioe_callback_t          cb[RK3506_BANK_NPINS];   /* per-pin callbacks */
  FAR void               *cb_arg[RK3506_BANK_NPINS];
#endif
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3506_gpio_bank_s g_rk3506_gpio[RK3506_GPIO_NBANKS] =
{
  { RK3506_GPIO0_ADDR, RK3506_IRQ_GPIO0_0, false },
  { RK3506_GPIO1_ADDR, RK3506_IRQ_GPIO1_0, false },
  { RK3506_GPIO2_ADDR, RK3506_IRQ_GPIO2_0, false },
  { RK3506_GPIO3_ADDR, RK3506_IRQ_GPIO3_0, false },
  { RK3506_GPIO4_ADDR, RK3506_IRQ_GPIO4_0, false },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_gpio_bank_check
 *
 * Description:
 *   Validate a global pin number and return its bank state, or NULL.
 *
 ****************************************************************************/

static FAR struct rk3506_gpio_bank_s *rk3506_gpio_bank_check(uint8_t gpin,
                                                            FAR uint8_t *bank,
                                                            FAR uint8_t *pin)
{
  uint8_t b = RK3506_GPIO_BANK(gpin);
  uint8_t p = RK3506_GPIO_PINNO(gpin);

  if (b >= RK3506_GPIO_NBANKS || p >= RK3506_BANK_NPINS)
    {
      return NULL;
    }

  *bank = b;
  *pin  = p;
  return &g_rk3506_gpio[b];
}

/****************************************************************************
 * Name: rk3506_gpio_bank_clock
 *
 * Description:
 *   Ungate a bank's PCLK + DBCLK exactly once.
 *
 ****************************************************************************/

static int rk3506_gpio_bank_clock(FAR struct rk3506_gpio_bank_s *priv,
                                  uint8_t bank)
{
  int ret;

  if (priv->clk_on)
    {
      return OK;
    }

  ret = rk3506_gpio_clock_enable(bank);
  if (ret == OK)
    {
      priv->clk_on = true;
    }

  return ret;
}

/****************************************************************************
 * Name: rk3506_gpio_set_dir
 *
 * Description:
 *   Program one pin's data direction (1 = output, 0 = input) with a
 *   single HIWORD store to SWPORT_DDR_L/H.
 *
 ****************************************************************************/

static void rk3506_gpio_set_dir(FAR struct rk3506_gpio_bank_s *priv,
                                uint8_t pin, bool output)
{
  uintptr_t reg = priv->base +
                  ((pin < 16u) ? GPIO_SWPORT_DDR_L : GPIO_SWPORT_DDR_H);
  uint32_t  bit = 1u << (pin & 0xfu);

  putreg32((bit << 16) | (output ? bit : 0u), reg);
}

/****************************************************************************
 * Name: rk3506_gpio_set_level
 *
 * Description:
 *   Drive one output pin with a single HIWORD store to SWPORT_DR_L/H.
 *
 ****************************************************************************/

static void rk3506_gpio_set_level(FAR struct rk3506_gpio_bank_s *priv,
                                  uint8_t pin, bool high)
{
  uintptr_t reg = priv->base +
                  ((pin < 16u) ? GPIO_SWPORT_DR_L : GPIO_SWPORT_DR_H);
  uint32_t  bit = 1u << (pin & 0xfu);

  putreg32((bit << 16) | (high ? bit : 0u), reg);
}

/****************************************************************************
 * Name: rk3506_gpio_get_level
 *
 * Description:
 *   Sample one pin's pad level from EXT_PORT.
 *
 ****************************************************************************/

static bool rk3506_gpio_get_level(FAR struct rk3506_gpio_bank_s *priv,
                                  uint8_t pin)
{
  return (getreg32(priv->base + GPIO_EXT_PORT) & (1u << pin)) != 0u;
}

#ifdef CONFIG_IOEXPANDER_INT_ENABLE

/****************************************************************************
 * Name: rk3506_gpio_set_trigger
 *
 * Description:
 *   Program the interrupt trigger type of one pin (INT_TYPE /
 *   INT_POLARITY / INT_BOTHEDGE), per rockchip_gpio_set_type().
 *
 ****************************************************************************/

static int rk3506_gpio_set_trigger(FAR struct rk3506_gpio_bank_s *priv,
                                   uint8_t pin, uint32_t trigger)
{
  uintptr_t base = priv->base;
  uint32_t  bit  = 1u << (pin & 0xfu);
  bool      high16 = (pin >= 16u);
  uintptr_t type_reg;
  uintptr_t pol_reg;
  uintptr_t both_reg;
  uint32_t  type;
  uint32_t  pol;
  uint32_t  both;

  type_reg = base + (high16 ? GPIO_INT_TYPE_H     : GPIO_INT_TYPE_L);
  pol_reg  = base + (high16 ? GPIO_INT_POLARITY_H : GPIO_INT_POLARITY_L);
  both_reg = base + (high16 ? GPIO_INT_BOTHEDGE_H : GPIO_INT_BOTHEDGE_L);

  switch (trigger)
    {
      case RK3506_TRIG_LEVEL_HIGH:
        type = 0u;
        pol  = 1u;
        both = 0u;
        break;

      case RK3506_TRIG_LEVEL_LOW:
        type = 0u;
        pol  = 0u;
        both = 0u;
        break;

      case RK3506_TRIG_EDGE_RISING:
        type = 1u;
        pol  = 1u;
        both = 0u;
        break;

      case RK3506_TRIG_EDGE_FALLING:
        type = 1u;
        pol  = 0u;
        both = 0u;
        break;

      case RK3506_TRIG_EDGE_BOTH:
        type = 1u;
        pol  = 0u;   /* ignored when BOTHEDGE=1 */
        both = 1u;
        break;

      default:
        return -EINVAL;
    }

  putreg32((bit << 16) | (type ? bit : 0u), type_reg);
  putreg32((bit << 16) | (pol  ? bit : 0u), pol_reg);
  putreg32((bit << 16) | (both ? bit : 0u), both_reg);

  return OK;
}

/****************************************************************************
 * Name: rk3506_gpio_irq_mask / rk3506_gpio_irq_unmask
 ****************************************************************************/

static void rk3506_gpio_irq_maskpin(FAR struct rk3506_gpio_bank_s *priv,
                                    uint8_t pin, bool mask)
{
  uintptr_t reg = priv->base +
                  ((pin < 16u) ? GPIO_INT_MASK_L : GPIO_INT_MASK_H);
  uint32_t  bit = 1u << (pin & 0xfu);

  putreg32((bit << 16) | (mask ? bit : 0u), reg);
}

/****************************************************************************
 * Name: rk3506_gpio_irq_ack
 *
 * Description:
 *   Acknowledge one pin's edge interrupt (write-1-to-clear PORT_EOI).
 *
 ****************************************************************************/

static void rk3506_gpio_irq_ack(FAR struct rk3506_gpio_bank_s *priv,
                                uint8_t pin)
{
  uintptr_t reg = priv->base +
                  ((pin < 16u) ? GPIO_PORT_EOI_L : GPIO_PORT_EOI_H);
  uint32_t  bit = 1u << (pin & 0xfu);

  putreg32((bit << 16) | bit, reg);
}

/****************************************************************************
 * Name: rk3506_gpio_interrupt
 *
 * Description:
 *   Combined GPIO bank ISR: demultiplex INT_STATUS, acknowledge edge
 *   interrupts (PORT_EOI) and dispatch the registered per-pin
 *   callbacks.  Mirrors rockchip_irq_demux() in gpio-rockchip.c.
 *
 ****************************************************************************/

static int rk3506_gpio_interrupt(int irq, FAR void *context,
                                 FAR void *arg)
{
  FAR struct rk3506_gpio_bank_s *priv =
    (FAR struct rk3506_gpio_bank_s *)arg;
  uint32_t status;
  uint8_t  pin;

  status = getreg32(priv->base + GPIO_INT_STATUS);

  while (status != 0u)
    {
      pin = (uint8_t)(__builtin_ffs((int)status) - 1);

      /* Acknowledge the edge interrupt before running the callback so
       * a level that is still asserted does not re-enter spuriously.
       */

      rk3506_gpio_irq_ack(priv, pin);

      if (priv->cb[pin] != NULL)
        {
          priv->cb[pin](&priv->ioe, (ioe_pinset_t)(1u << pin),
                        priv->cb_arg[pin]);
        }

      status &= ~(1u << pin);
    }

  return OK;
}
#endif /* CONFIG_IOEXPANDER_INT_ENABLE */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_gpio_config
 ****************************************************************************/

int rk3506_gpio_config(uint8_t gpin, uint32_t flags)
{
  FAR struct rk3506_gpio_bank_s *priv;
  uint8_t bank;
  uint8_t pin;
  int     ret;

  priv = rk3506_gpio_bank_check(gpin, &bank, &pin);
  if (priv == NULL)
    {
      return -EINVAL;
    }

  /* 1. Ungate the bank clocks (PCLK + DBCLK). */

  ret = rk3506_gpio_bank_clock(priv, bank);
  if (ret != OK)
    {
      return ret;
    }

  /* 2. Mux the pin to GPIO function 0 (rk3506_pinctrl_set_mux). */

  ret = rk3506_pinctrl_set_mux(bank, pin, 0u);
  if (ret != OK)
    {
      return ret;
    }

  /* 3. Pull configuration. */

  if ((flags & RK3506_GPIO_PULL_UP) != 0u)
    {
      rk3506_pinctrl_set_pull(bank, pin, RK3506_PULL_UP);
    }
  else if ((flags & RK3506_GPIO_PULL_DOWN) != 0u)
    {
      rk3506_pinctrl_set_pull(bank, pin, RK3506_PULL_DOWN);
    }
  else
    {
      rk3506_pinctrl_set_pull(bank, pin, RK3506_PULL_DISABLE);
    }

  /* 4. Direction + initial level.  For an output, program the level
   *    before enabling the driver to avoid a glitch.
   */

  if ((flags & RK3506_GPIO_OUTPUT) != 0u)
    {
      rk3506_gpio_set_level(priv, pin,
                            (flags & RK3506_GPIO_VALUE_HIGH) != 0u);
      rk3506_gpio_set_dir(priv, pin, true);
    }
  else
    {
      rk3506_gpio_set_dir(priv, pin, false);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3506_gpio_write
 ****************************************************************************/

int rk3506_gpio_write(uint8_t gpin, bool value)
{
  FAR struct rk3506_gpio_bank_s *priv;
  uint8_t bank;
  uint8_t pin;

  priv = rk3506_gpio_bank_check(gpin, &bank, &pin);
  if (priv == NULL)
    {
      return -EINVAL;
    }

  rk3506_gpio_set_level(priv, pin, value);
  return OK;
}

/****************************************************************************
 * Name: rk3506_gpio_read
 ****************************************************************************/

int rk3506_gpio_read(uint8_t gpin, FAR bool *value)
{
  FAR struct rk3506_gpio_bank_s *priv;
  uint8_t bank;
  uint8_t pin;

  priv = rk3506_gpio_bank_check(gpin, &bank, &pin);
  if (priv == NULL || value == NULL)
    {
      return -EINVAL;
    }

  *value = rk3506_gpio_get_level(priv, pin);
  return OK;
}

/****************************************************************************
 * ioexpander lower-half operations
 ****************************************************************************/

#ifdef CONFIG_IOEXPANDER

static int rk3506_ioe_direction(FAR struct ioexpander_dev_s *dev,
                                uint8_t pin, int direction)
{
  FAR struct rk3506_gpio_bank_s *priv =
    (FAR struct rk3506_gpio_bank_s *)dev;
  uint8_t bank = (uint8_t)(priv - &g_rk3506_gpio[0]);
  int     ret;

  if (pin >= RK3506_BANK_NPINS)
    {
      return -EINVAL;
    }

  ret = rk3506_gpio_bank_clock(priv, bank);
  if (ret != OK)
    {
      return ret;
    }

  /* Make sure the pin is muxed to GPIO function 0 first. */

  ret = rk3506_pinctrl_set_mux(bank, pin, 0u);
  if (ret != OK)
    {
      return ret;
    }

  switch (direction)
    {
      case IOEXPANDER_DIRECTION_IN:
        rk3506_pinctrl_set_pull(bank, pin, RK3506_PULL_DISABLE);
        rk3506_gpio_set_dir(priv, pin, false);
        break;

      case IOEXPANDER_DIRECTION_IN_PULLUP:
        rk3506_pinctrl_set_pull(bank, pin, RK3506_PULL_UP);
        rk3506_gpio_set_dir(priv, pin, false);
        break;

      case IOEXPANDER_DIRECTION_IN_PULLDOWN:
        rk3506_pinctrl_set_pull(bank, pin, RK3506_PULL_DOWN);
        rk3506_gpio_set_dir(priv, pin, false);
        break;

      case IOEXPANDER_DIRECTION_OUT:
      case IOEXPANDER_DIRECTION_OUT_LED:
        /* The RK3506 GPIO has no open-drain drive mode at the
         * controller level (open-drain would be emulated in software),
         * so OUT and OUT_LED both map to push-pull.
         */

        rk3506_gpio_set_dir(priv, pin, true);
        break;

      case IOEXPANDER_DIRECTION_OUT_OPENDRAIN:
        return -ENOTSUP;

      default:
        return -EINVAL;
    }

  return OK;
}

static int rk3506_ioe_option(FAR struct ioexpander_dev_s *dev,
                             uint8_t pin, int opt, FAR void *val)
{
  FAR struct rk3506_gpio_bank_s *priv =
    (FAR struct rk3506_gpio_bank_s *)dev;

  if (pin >= RK3506_BANK_NPINS)
    {
      return -EINVAL;
    }

#ifdef CONFIG_IOEXPANDER_INT_ENABLE
  if (opt == IOEXPANDER_OPTION_INTCFG)
    {
      uint32_t trigger;
      int      cfg = (int)(intptr_t)val;

      switch (cfg)
        {
          case IOEXPANDER_VAL_HIGH:
            trigger = RK3506_TRIG_LEVEL_HIGH;
            break;

          case IOEXPANDER_VAL_LOW:
            trigger = RK3506_TRIG_LEVEL_LOW;
            break;

          case IOEXPANDER_VAL_RISING:
            trigger = RK3506_TRIG_EDGE_RISING;
            break;

          case IOEXPANDER_VAL_FALLING:
            trigger = RK3506_TRIG_EDGE_FALLING;
            break;

          case IOEXPANDER_VAL_BOTH:
            trigger = RK3506_TRIG_EDGE_BOTH;
            break;

          case IOEXPANDER_VAL_DISABLE:
          default:
            rk3506_gpio_irq_maskpin(priv, pin, true);
            return OK;
        }

      return rk3506_gpio_set_trigger(priv, pin, trigger);
    }
#endif

  /* INVERT / LEDCFG / WAKEUPCFG / SETDEBOUNCE / SETMASK are not
   * supported by the RK3506 GPIO controller (no per-pin inversion or
   * LED mux; debounce is global rather than per-pin here).
   */

  return -ENOTSUP;
}

static int rk3506_ioe_writepin(FAR struct ioexpander_dev_s *dev,
                               uint8_t pin, bool value)
{
  FAR struct rk3506_gpio_bank_s *priv =
    (FAR struct rk3506_gpio_bank_s *)dev;

  if (pin >= RK3506_BANK_NPINS)
    {
      return -EINVAL;
    }

  rk3506_gpio_set_level(priv, pin, value);
  return OK;
}

static int rk3506_ioe_readpin(FAR struct ioexpander_dev_s *dev,
                              uint8_t pin, FAR bool *value)
{
  FAR struct rk3506_gpio_bank_s *priv =
    (FAR struct rk3506_gpio_bank_s *)dev;

  if (pin >= RK3506_BANK_NPINS || value == NULL)
    {
      return -EINVAL;
    }

  *value = rk3506_gpio_get_level(priv, pin);
  return OK;
}

static int rk3506_ioe_readbuf(FAR struct ioexpander_dev_s *dev,
                              uint8_t pin, FAR bool *value)
{
  /* There is no separate output-latch readback register distinct from
   * EXT_PORT on this IP (the pad value is what the Linux driver also
   * returns), so readbuf aliases readpin.
   */

  return rk3506_ioe_readpin(dev, pin, value);
}

#ifdef CONFIG_IOEXPANDER_INT_ENABLE

static FAR void *rk3506_ioe_attach(FAR struct ioexpander_dev_s *dev,
                                   ioe_pinset_t pinset,
                                   ioe_callback_t callback,
                                   FAR void *arg)
{
  FAR struct rk3506_gpio_bank_s *priv =
    (FAR struct rk3506_gpio_bank_s *)dev;
  uint8_t bank = (uint8_t)(priv - &g_rk3506_gpio[0]);
  uint8_t pin;
  int     ret;

  /* This ioexpander handles one pin per node (gpio_lower_half passes a
   * single-bit pinset), so decode it.
   */

  if (pinset == 0 || (pinset & (pinset - 1u)) != 0u)
    {
      return NULL;   /* only single-pin attach is supported */
    }

  pin = (uint8_t)(__builtin_ffs((int)pinset) - 1);
  if (pin >= RK3506_BANK_NPINS)
    {
      return NULL;
    }

  ret = rk3506_gpio_bank_clock(priv, bank);
  if (ret != OK)
    {
      return NULL;
    }

  priv->cb[pin]     = callback;
  priv->cb_arg[pin] = arg;

  /* Route the bank interrupt to the GIC handler (idempotent) and
   * unmask the pin.  The trigger type is programmed separately through
   * the IOEXPANDER_OPTION_INTCFG option.
   */

  irq_attach(priv->irq, rk3506_gpio_interrupt, priv);
  up_enable_irq(priv->irq);
  rk3506_gpio_irq_maskpin(priv, pin, false);

  /* Return the pin number as the opaque handle (offset by 1 so that
   * pin 0 yields a non-NULL handle).
   */

  return (FAR void *)(intptr_t)(pin + 1);
}

static int rk3506_ioe_detach(FAR struct ioexpander_dev_s *dev,
                             FAR void *handle)
{
  FAR struct rk3506_gpio_bank_s *priv =
    (FAR struct rk3506_gpio_bank_s *)dev;
  uint8_t pin = (uint8_t)((intptr_t)handle - 1);

  if (handle == NULL || pin >= RK3506_BANK_NPINS)
    {
      return -EINVAL;
    }

  rk3506_gpio_irq_maskpin(priv, pin, true);
  priv->cb[pin]     = NULL;
  priv->cb_arg[pin] = NULL;

  return OK;
}
#endif /* CONFIG_IOEXPANDER_INT_ENABLE */

static const struct ioexpander_ops_s g_rk3506_ioe_ops =
{
  rk3506_ioe_direction,   /* ioe_direction */
  rk3506_ioe_option,      /* ioe_option    */
  rk3506_ioe_writepin,    /* ioe_writepin  */
  rk3506_ioe_readpin,     /* ioe_readpin   */
  rk3506_ioe_readbuf,     /* ioe_readbuf   */
#ifdef CONFIG_IOEXPANDER_INT_ENABLE
  rk3506_ioe_attach,      /* ioe_attach    */
  rk3506_ioe_detach,      /* ioe_detach    */
#endif
};

/****************************************************************************
 * Name: rk3506_gpio_bank_initialize
 ****************************************************************************/

FAR struct ioexpander_dev_s *rk3506_gpio_bank_initialize(uint8_t bank)
{
  FAR struct rk3506_gpio_bank_s *priv;
  int ret;

  if (bank >= RK3506_GPIO_NBANKS)
    {
      return NULL;
    }

  priv = &g_rk3506_gpio[bank];

  ret = rk3506_gpio_bank_clock(priv, bank);
  if (ret != OK)
    {
      return NULL;
    }

  if (priv->ioe.ops == NULL)
    {
      priv->ioe.ops = &g_rk3506_ioe_ops;
    }

  return &priv->ioe;
}
#endif /* CONFIG_IOEXPANDER */

/****************************************************************************
 * Chip-level GPIO interrupt support
 ****************************************************************************/

#ifdef CONFIG_IOEXPANDER_INT_ENABLE

int rk3506_gpio_irq_config(uint8_t gpin, uint32_t trigger)
{
  FAR struct rk3506_gpio_bank_s *priv;
  uint8_t bank;
  uint8_t pin;
  int     ret;

  priv = rk3506_gpio_bank_check(gpin, &bank, &pin);
  if (priv == NULL)
    {
      return -EINVAL;
    }

  ret = rk3506_gpio_bank_clock(priv, bank);
  if (ret != OK)
    {
      return ret;
    }

  return rk3506_gpio_set_trigger(priv, pin, trigger);
}

int rk3506_gpio_irq_attach(uint8_t gpin, ioe_callback_t callback,
                           FAR void *arg)
{
  FAR struct rk3506_gpio_bank_s *priv;
  uint8_t bank;
  uint8_t pin;
  int     ret;

  priv = rk3506_gpio_bank_check(gpin, &bank, &pin);
  if (priv == NULL)
    {
      return -EINVAL;
    }

  ret = rk3506_gpio_bank_clock(priv, bank);
  if (ret != OK)
    {
      return ret;
    }

  priv->cb[pin]     = callback;
  priv->cb_arg[pin] = arg;

  irq_attach(priv->irq, rk3506_gpio_interrupt, priv);
  up_enable_irq(priv->irq);

  return OK;
}

int rk3506_gpio_irq_enable(uint8_t gpin, bool enable)
{
  FAR struct rk3506_gpio_bank_s *priv;
  uint8_t bank;
  uint8_t pin;

  priv = rk3506_gpio_bank_check(gpin, &bank, &pin);
  if (priv == NULL)
    {
      return -EINVAL;
    }

  rk3506_gpio_irq_maskpin(priv, pin, !enable);
  return OK;
}
#endif /* CONFIG_IOEXPANDER_INT_ENABLE */
