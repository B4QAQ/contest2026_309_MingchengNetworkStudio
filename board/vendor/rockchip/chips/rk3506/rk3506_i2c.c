/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_i2c.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations under
 * the License.
 *
 * Rewrite v2 (2026-08-27):
 *   - Follows Linux kernel i2c-rk3x.c pattern (state machine + interrupt/polling)
 *   - Fixes bugs from previous version (wait_bus_free checked wrong bit)
 *   - Uses proper combined write/read transaction for SMBus-style reads
 *   - Tested at compile level only; requires hardware for runtime validation
 *
 * Rewrite v3 (transfer engine aligned to the vendor RTOS HAL):
 *   - Register sequence follows the SDK HAL verbatim:
 *       hal/lib/hal/src/hal_i2c.c (I2C_FillTransmitBuf L291-323,
 *       I2C_PrepareRead L255-283, I2C_HandleRead L404-441,
 *       I2C_Start/I2C_Stop L182-247, HAL_I2C_IRQHandler L563-627,
 *       HAL_I2C_ForceStop L707-719)
 *   - TXDATA/RXDATA are 8 x 32-bit words, 4 bytes packed per word;
 *     MTXCNT counts address + data bytes and is set before START.
 *   - Message planning follows rtos/bsp/rockchip/common/drivers/drv_i2c.c
 *     L99-145 (combined register-address write + read in MOD_REG_TX).
 *   - Polling waits run OUTSIDE spin_lock_irqsave critical sections.
 *   - No I2C device on the bench: correct per SDK, not hardware verified.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>
#include <nuttx/i2c/i2c_master.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_i2c.h"
#include "rk3506_cru.h"
#include "rk3506_register.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* I2C bus configuration */

#define RK3506_I2C_NUM_BUS       3

/* I2C bus default speed */

#define I2C_SPEED_DEFAULT        100000   /* 100 kHz */
#define I2C_SPEED_STANDARD       100000
#define I2C_SPEED_FAST           400000
#define I2C_SPEED_FAST_PLUS      1000000

/* I2C_CLKDIV register fields (rk3506.h:3342-3345): the divider is split
 * into CLKDIVL[15:0] (SCL low period) and CLKDIVH[31:16] (SCL high
 * period), SCL = pclk / (8 * (CLKDIVL+1 + CLKDIVH+1)).
 */

#define I2C_CLKDIV_CLKDIVL_SHIFT 0u
#define I2C_CLKDIV_CLKDIVH_SHIFT 16u

/* I2C timing spec per bus speed (hal_i2c.c struct I2C_SPEC_VALUES,
 * I2C_GetSpec): used to split the divider so both the SCL low and high
 * periods meet the I2C tLOW/tHIGH (+tRise/tFall) minimums.
 */

struct rk3506_i2c_spec_s
{
  uint32_t min_low_ns;
  uint32_t min_high_ns;
  uint32_t max_rise_ns;
  uint32_t max_fall_ns;
};

static const struct rk3506_i2c_spec_s g_i2c_standard_spec =
{
  .min_low_ns  = 4700,
  .min_high_ns = 4000,
  .max_rise_ns = 1000,
  .max_fall_ns = 300,
};

static const struct rk3506_i2c_spec_s g_i2c_fast_spec =
{
  .min_low_ns  = 1300,
  .min_high_ns = 600,
  .max_rise_ns = 300,
  .max_fall_ns = 300,
};

static const struct rk3506_i2c_spec_s g_i2c_fast_plus_spec =
{
  .min_low_ns  = 500,
  .min_high_ns = 260,
  .max_rise_ns = 120,
  .max_fall_ns = 120,
};

/* Per-transfer timeout (ms) */

#define RK3506_I2C_TIMEOUT_MS    200

/* Number of 32-bit TXDATA/RXDATA registers (rk3506.h TXDATA0..7/RXDATA0..7,
 * each packing 4 bytes -> 32 bytes per FIFO chunk).
 */

#define RK3506_I2C_FIFO_WORDS    8

/* eI2C_Mode mode NUMBERS from hal_i2c.h L47-52.  They occupy CON[2:1] and
 * are written as (mode << 1).
 */

#define RK3506_I2C_MODE_TX       0        /* REG_CON_MOD_TX: transmit data */
#define RK3506_I2C_MODE_REG_TX   1        /* select register and restart */
#define RK3506_I2C_MODE_RX       2        /* REG_CON_MOD_RX: receive data */

/* Result of one polling step through the transfer state machine */

#define RK3506_I2C_XFER_BUSY     0        /* Still in progress */
#define RK3506_I2C_XFER_DONE     1        /* Segment finished */

/* I2C controller states (matches eI2C_State in hal_i2c.h L74-80) */

enum rk3506_i2c_state_e
{
  RK3506_I2C_STATE_IDLE = 0,
  RK3506_I2C_STATE_START,        /* START condition sent */
  RK3506_I2C_STATE_READ,         /* Master receiver */
  RK3506_I2C_STATE_WRITE,        /* Master transmitter */
  RK3506_I2C_STATE_STOP          /* STOP condition sent */
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* I2C bus private data */

struct rk3506_i2c_dev_s
{
  struct i2c_master_s  dev;      /* NuttX I2C master device */
  spinlock_t           lock;     /* Bus lock */
  uint32_t             base;     /* Register base address */
  int                  irq;      /* IRQ number */
  int                  bus;      /* Bus number */
  uint32_t             clkrate;  /* I2C bus clock rate (Hz) */

  /* Current transfer state (polling mode) */

  volatile int         state;    /* One of rk3506_i2c_state_e */
  volatile int         error;    /* Last error code */
  volatile bool        busy;     /* True while transfer in progress */

  FAR struct i2c_msg_s *msg;     /* Current message being processed */
  int                   processed;  /* Bytes processed in current message */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3506_i2c_dev_s g_i2c_dev[RK3506_I2C_NUM_BUS];

/* I2C base addresses */

static const uint32_t g_i2c_base[RK3506_I2C_NUM_BUS] =
{
  RK3506_I2C0_ADDR,
  RK3506_I2C1_ADDR,
  RK3506_I2C2_ADDR
};

/* I2C IRQ numbers */

static const int g_i2c_irq[RK3506_I2C_NUM_BUS] =
{
  RK3506_IRQ_I2C0,
  RK3506_IRQ_I2C1,
  RK3506_IRQ_I2C2
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  rk3506_i2c_wait_bus_not_busy(struct rk3506_i2c_dev_s *priv);
static void rk3506_i2c_fill_tx(struct rk3506_i2c_dev_s *priv);
static void rk3506_i2c_prepare_read(struct rk3506_i2c_dev_s *priv);
static int  rk3506_i2c_request_stop(struct rk3506_i2c_dev_s *priv, int error,
                                    bool islast);
static int  rk3506_i2c_handle_start(struct rk3506_i2c_dev_s *priv, int mode);
static int  rk3506_i2c_handle_write(struct rk3506_i2c_dev_s *priv,
                                    bool islast);
static int  rk3506_i2c_handle_read(struct rk3506_i2c_dev_s *priv,
                                   bool islast);
static int  rk3506_i2c_handle_stop(struct rk3506_i2c_dev_s *priv);
static int  rk3506_i2c_irq_step(struct rk3506_i2c_dev_s *priv, int mode,
                                bool islast);
static void rk3506_i2c_start(struct rk3506_i2c_dev_s *priv, int mode);
static void rk3506_i2c_force_stop(struct rk3506_i2c_dev_s *priv);
static void rk3506_i2c_disable(struct rk3506_i2c_dev_s *priv);
static int  rk3506_i2c_prepare_segment(struct rk3506_i2c_dev_s *priv,
                                       FAR struct i2c_msg_s *msgs,
                                       int remaining, FAR int *mode);
static int  rk3506_i2c_run_segment(struct rk3506_i2c_dev_s *priv, int mode,
                                   bool islast);
static int  rk3506_i2c_xfer_polling(struct rk3506_i2c_dev_s *priv,
                                    FAR struct i2c_msg_s *msgs, int count);
static void rk3506_i2c_set_clk(struct rk3506_i2c_dev_s *priv,
                               uint32_t clkrate);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_i2c_getreg / putreg
 ****************************************************************************/

static inline uint32_t rk3506_i2c_getreg(struct rk3506_i2c_dev_s *priv,
                                          uint32_t offset)
{
  return getreg32(priv->base + offset);
}

static inline void rk3506_i2c_putreg(struct rk3506_i2c_dev_s *priv,
                                      uint32_t offset, uint32_t val)
{
  putreg32(val, priv->base + offset);
}

/****************************************************************************
 * Name: rk3506_i2c_set_clk
 *
 * Description:
 *   Set the I2C bus clock rate.
 *
 *   Per RK3x reference (drivers/i2c/rk_i2c.c, rk_i2c_set_clk):
 *     div = DIV_ROUND_UP(i2c_rate, scl_rate * 8) - 2;
 *
 *   For RK3506 with PCLK = 24 MHz and SCL = 100 kHz:
 *     div = DIV_ROUND_UP(24 MHz, 100 kHz * 8) - 2
 *         = DIV_ROUND_UP(24 MHz, 800 kHz) - 2
 *         = 30 - 2 = 28
 *
 *   The previous formula `(PCLK / (8 * desired_freq)) - 1` was off by 1,
 *   producing a baud rate that's about 4% slow.
 *
 *   Note: The v0 (rk3288) variant uses min_t/mh/mh separate dividers, but
 *   for v1+ and RK3506, a single `div` is sufficient.
 *
 ****************************************************************************/

static void rk3506_i2c_set_clk(struct rk3506_i2c_dev_s *priv,
                               uint32_t clkrate)
{
  uint32_t pclk = 24000000;  /* CLK_I2Cx functional clock (see rk3506_cru.c:
                              * the mux/divider is left at the reset parent
                              * xin24m), NOT PCLK_APB. */
  const struct rk3506_i2c_spec_s *spec;
  uint32_t rate_khz;
  uint32_t speed_khz;
  uint32_t min_total_div;
  uint32_t min_high_div;
  uint32_t min_low_div;
  uint32_t min_hold_div;
  uint32_t high_div;
  uint32_t low_div;
  uint32_t clkdiv;

  if (clkrate == 0)
    {
      clkrate = I2C_SPEED_DEFAULT;
    }

  if (clkrate > I2C_SPEED_FAST_PLUS)
    {
      clkrate = I2C_SPEED_FAST_PLUS;
    }

  /* Pick the timing spec matching the bus speed (hal_i2c.c I2C_GetSpec). */

  if (clkrate >= I2C_SPEED_FAST_PLUS)
    {
      spec = &g_i2c_fast_plus_spec;
    }
  else if (clkrate >= I2C_SPEED_FAST)
    {
      spec = &g_i2c_fast_spec;
    }
  else
    {
      spec = &g_i2c_standard_spec;
    }

  /* Divider split per hal_i2c.c HAL_I2C_AdaptDIV.  The single-value
   * formula DIV_ROUND_UP(pclk, 8*scl) - 2 only ever set CLKDIVL and left
   * CLKDIVH = 0, giving a ~333 ns SCL high period that violates the I2C
   * tHIGH minimum in every mode; the high and low periods must be
   * divided separately.
   */

  rate_khz  = (pclk + 999u) / 1000u;              /* HAL_DIV_ROUND_UP */
  speed_khz = (clkrate + 999u) / 1000u;

  min_total_div = (rate_khz + speed_khz * 8u - 1u) / (speed_khz * 8u);

  min_high_div = (rate_khz * (spec->max_rise_ns + spec->min_high_ns) +
                  8u * 1000000u - 1u) / (8u * 1000000u);
  min_low_div  = (rate_khz * (spec->max_fall_ns + spec->min_low_ns) +
                  8u * 1000000u - 1u) / (8u * 1000000u);

  if (min_high_div < 1u)
    {
      min_high_div = 2u;
    }

  if (min_low_div < 1u)
    {
      min_low_div = 2u;
    }

  min_hold_div = min_high_div + min_low_div;

  if (min_hold_div >= min_total_div)
    {
      high_div = min_high_div;
      low_div  = min_low_div;
    }
  else
    {
      uint32_t extra_div     = min_total_div - min_hold_div;
      uint32_t extra_low_div = (min_low_div * extra_div +
                                min_hold_div - 1u) / min_hold_div;

      low_div  = min_low_div + extra_low_div;
      high_div = min_high_div + (extra_div - extra_low_div);
    }

  /* The register stores div-1 in each field. */

  high_div--;
  low_div--;

  if (high_div > 0xffffu)
    {
      high_div = 0xffffu;
    }

  if (low_div > 0xffffu)
    {
      low_div = 0xffffu;
    }

  clkdiv = (high_div << I2C_CLKDIV_CLKDIVH_SHIFT) |
           (low_div  << I2C_CLKDIV_CLKDIVL_SHIFT);

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CLKDIV, clkdiv);
  priv->clkrate = clkrate;

  i2cinfo("I2C%d: clkrate=%u Hz, clkdiv=0x%08lx (H=%lu L=%lu, pclk=24MHz)\n",
          priv->bus, (unsigned int)clkrate, (unsigned long)clkdiv,
          (unsigned long)high_div, (unsigned long)low_div);
}

/****************************************************************************
 * Name: rk3506_i2c_wait_bus_not_busy
 *
 * Description:
 *   Wait until the I2C bus is no longer busy.
 *
 *   IMPORTANT: This waits for CON_EN to clear, which happens automatically
 *   when the controller is idle (no transfer in progress). The previous
 *   implementation incorrectly waited for CON_EN to be set, which is the
 *   opposite of what we want.
 *
 *   Called with interrupts enabled: this is a bounded wait and must never
 *   run inside a spin_lock_irqsave() critical section.
 *
 ****************************************************************************/

static int rk3506_i2c_wait_bus_not_busy(struct rk3506_i2c_dev_s *priv)
{
  uint32_t timeout = RK3506_I2C_TIMEOUT_MS * 100;  /* 10 us units */

  while (rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON) & RK3506_I2C_CON_EN)
    {
      if (--timeout == 0)
        {
          _err("ERROR: I2C%d bus stuck (CON=0x%08x)\n",
               priv->bus,
               rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON));
          return -ETIMEDOUT;
        }

      up_udelay(10);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3506_i2c_clean_ipd
 *
 * Description:
 *   Clear all pending interrupt status bits (hal_i2c.c I2C_CleanIPD L141).
 *
 ****************************************************************************/

static inline void rk3506_i2c_clean_ipd(struct rk3506_i2c_dev_s *priv)
{
  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_ALL);
}

/****************************************************************************
 * Name: rk3506_i2c_fill_tx
 *
 * Description:
 *   Fill the transmit FIFO for one chunk.
 *
 *   Verbatim port of I2C_FillTransmitBuf() (hal_i2c.c L291-323):
 *   - TXDATA is 8 words x 32 bit; each word packs 4 little-endian bytes
 *     (val |= byte << (j * 8)).
 *   - The very first byte of the first chunk is the slave address
 *     (addr & 0x7f) << 1; the caller selects MOD_RX/MOD_REG_TX for reads,
 *     so a write chunk always carries the write address.
 *   - MTXCNT is the number of bytes to shift out for THIS chunk
 *     (address + data, at most 32) and is written before START consumes it.
 *
 ****************************************************************************/

static void rk3506_i2c_fill_tx(struct rk3506_i2c_dev_s *priv)
{
  unsigned int i;
  unsigned int j;
  uint32_t val;
  uint32_t cnt = 0;
  uint8_t byte;
  uint16_t len = priv->msg->length;
  uint16_t addr = priv->msg->addr;

  for (i = 0; i < RK3506_I2C_FIFO_WORDS; ++i)
    {
      val = 0;
      for (j = 0; j < 4; ++j)
        {
          if ((priv->processed == len) && (cnt != 0))
            {
              break;
            }

          if (priv->processed == 0 && cnt == 0)
            {
              byte = (addr & 0x7f) << 1;
            }
          else
            {
              byte = priv->msg->buffer[priv->processed++];
            }

          val |= (uint32_t)byte << (j * 8);
          cnt++;
        }

      rk3506_i2c_putreg(priv, RK3506_I2C_TXBUFFER_BASE + i * 4, val);
      if (priv->processed == len)
        {
          break;
        }
    }

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_MTXCNT, cnt);
}

/****************************************************************************
 * Name: rk3506_i2c_prepare_read
 *
 * Description:
 *   Arm the receive engine for one chunk.
 *
 *   Verbatim port of I2C_PrepareRead() (hal_i2c.c L255-283): at most
 *   32 bytes per chunk; LASTACK is set only for the final chunk (NACK the
 *   last byte so the following STOP is legal); every continuation chunk is
 *   forced into plain MOD_RX.
 *
 ****************************************************************************/

static void rk3506_i2c_prepare_read(struct rk3506_i2c_dev_s *priv)
{
  uint32_t len = (uint32_t)priv->msg->length - priv->processed;
  uint32_t con;

  con = rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON);

  if (len > RK3506_I2C_MAX_RXBUF)
    {
      len = RK3506_I2C_MAX_RXBUF;
      con &= ~RK3506_I2C_CON_LASTACK;
    }
  else
    {
      con |= RK3506_I2C_CON_LASTACK;
    }

  /* Make sure we are in plain RX mode if we read a second chunk */

  if (priv->processed != 0)
    {
      con &= ~RK3506_I2C_CON_MOD_MASK;
      con |= RK3506_I2C_CON_MOD_RX;
    }

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON, con);
  rk3506_i2c_putreg(priv, RK3506_I2C_REG_MRXCNT, len);
}

/****************************************************************************
 * Name: rk3506_i2c_request_stop
 *
 * Description:
 *   Finish the current message.
 *
 *   Port of I2C_Stop() (hal_i2c.c L213-247).  For the last message (or on
 *   error) arm CON_STOP and enter STATE_STOP; between messages the IP is
 *   disabled and the next message starts with an ordinary START because the
 *   hardware cannot emit a true repeated START here.
 *
 * Returned Value:
 *   RK3506_I2C_XFER_BUSY - STOP phase started, wait for INT_STOP
 *   RK3506_I2C_XFER_DONE - controller disabled, caller may start next msg
 *
 ****************************************************************************/

static int rk3506_i2c_request_stop(struct rk3506_i2c_dev_s *priv, int error,
                                   bool islast)
{
  uint32_t ctrl;

  priv->processed = 0;
  priv->error = error;

  if (islast || error)
    {
      priv->state = RK3506_I2C_STATE_STOP;

      ctrl = rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON);
      ctrl |= RK3506_I2C_CON_STOP;
      rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON, ctrl);

      return RK3506_I2C_XFER_BUSY;
    }

  /* To start the next message.
   *
   * The HW is actually not capable of REPEATED START. But we can get the
   * intended effect by resetting its internal state and issuing an ordinary
   * START (hal_i2c.c L237-243).
   *
   * TODO: needs hardware validation for multi-message transactions that do
   * NOT match the fused register-read shape (the GT911 style write-subaddr
   * + read is instead handled in hardware via MOD_REG_TX): this sequence
   * releases the bus between messages, so it is STOP+START rather than a
   * true repeated start.  No I2C device on the bench to verify.
   */

  priv->state = RK3506_I2C_STATE_IDLE;

  ctrl = rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON) &
         RK3506_I2C_CON_TUNING_MASK;
  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON, ctrl);

  return RK3506_I2C_XFER_DONE;
}

/****************************************************************************
 * Name: rk3506_i2c_handle_start
 *
 * Description:
 *   Service the START interrupt (hal_i2c.c I2C_HandleStart L332-365):
 *   clear START, then either fill TX for a write or arm the receive engine.
 *
 ****************************************************************************/

static int rk3506_i2c_handle_start(struct rk3506_i2c_dev_s *priv, int mode)
{
  if (!(rk3506_i2c_getreg(priv, RK3506_I2C_REG_IPD) &
        RK3506_I2C_INT_START))
    {
      _err("ERROR: I2C%d unexpected irq in START (IPD=0x%08x)\n",
           priv->bus, rk3506_i2c_getreg(priv, RK3506_I2C_REG_IPD));
      rk3506_i2c_clean_ipd(priv);
      return rk3506_i2c_request_stop(priv, -EIO, true);
    }

  /* Ack interrupt and disable start bit */

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_START);
  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON,
                    rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON) &
                    ~RK3506_I2C_CON_START);

  if (mode == RK3506_I2C_MODE_TX)
    {
      priv->state = RK3506_I2C_STATE_WRITE;
      rk3506_i2c_fill_tx(priv);
    }
  else
    {
      /* In any other case, we are going to be reading */

      priv->state = RK3506_I2C_STATE_READ;
      rk3506_i2c_prepare_read(priv);
    }

  return RK3506_I2C_XFER_BUSY;
}

/****************************************************************************
 * Name: rk3506_i2c_handle_write
 *
 * Description:
 *   Service a master-transmit-finished interrupt (hal_i2c.c
 *   I2C_HandleWrite L374-395): refill the TX FIFO for the next chunk or
 *   stop when all data has been shifted out.
 *
 ****************************************************************************/

static int rk3506_i2c_handle_write(struct rk3506_i2c_dev_s *priv,
                                   bool islast)
{
  if (!(rk3506_i2c_getreg(priv, RK3506_I2C_REG_IPD) &
        RK3506_I2C_INT_MBTF))
    {
      _err("ERROR: I2C%d unexpected irq in WRITE (IPD=0x%08x)\n",
           priv->bus, rk3506_i2c_getreg(priv, RK3506_I2C_REG_IPD));
      rk3506_i2c_clean_ipd(priv);
      return rk3506_i2c_request_stop(priv, -EIO, true);
    }

  /* Ack interrupt */

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_MBTF);

  /* Are we finished? */

  if (priv->processed == priv->msg->length)
    {
      return rk3506_i2c_request_stop(priv, priv->error, islast);
    }

  rk3506_i2c_fill_tx(priv);
  return RK3506_I2C_XFER_BUSY;
}

/****************************************************************************
 * Name: rk3506_i2c_handle_read
 *
 * Description:
 *   Service a master-receive-finished interrupt (hal_i2c.c
 *   I2C_HandleRead L404-441): unpack at most 32 bytes from the 32-bit
 *   RXDATA words (RXDATA[i/4], lane i%4) and either arm the next chunk or
 *   stop.
 *
 ****************************************************************************/

static int rk3506_i2c_handle_read(struct rk3506_i2c_dev_s *priv,
                                  bool islast)
{
  uint32_t len = (uint32_t)priv->msg->length - priv->processed;
  unsigned int i;
  uint32_t val = 0;
  uint8_t byte;

  /* We only care for MBRF here */

  if (!(rk3506_i2c_getreg(priv, RK3506_I2C_REG_IPD) &
        RK3506_I2C_INT_MBRF))
    {
      return RK3506_I2C_XFER_BUSY;
    }

  /* Ack interrupt */

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_MBRF);

  /* Can only handle a maximum of 32 bytes at a time */

  if (len > RK3506_I2C_MAX_RXBUF)
    {
      len = RK3506_I2C_MAX_RXBUF;
    }

  /* Read the data from receive buffer */

  for (i = 0; i < len; ++i)
    {
      if (i % 4 == 0)
        {
          val = rk3506_i2c_getreg(priv, RK3506_I2C_RXBUFFER_BASE +
                                  (i / 4) * 4);
        }

      byte = (val >> ((i % 4) * 8)) & 0xff;
      priv->msg->buffer[priv->processed++] = byte;
    }

  /* Are we finished? */

  if (priv->processed == priv->msg->length)
    {
      return rk3506_i2c_request_stop(priv, priv->error, islast);
    }

  rk3506_i2c_prepare_read(priv);
  return RK3506_I2C_XFER_BUSY;
}

/****************************************************************************
 * Name: rk3506_i2c_handle_stop
 *
 * Description:
 *   Service the STOP interrupt (hal_i2c.c I2C_HandleStop L450-473): clear
 *   the STOP bit and report completion.  The final error code lives in
 *   priv->error (set by rk3506_i2c_request_stop).
 *
 ****************************************************************************/

static int rk3506_i2c_handle_stop(struct rk3506_i2c_dev_s *priv)
{
  uint32_t con;

  if (!(rk3506_i2c_getreg(priv, RK3506_I2C_REG_IPD) &
        RK3506_I2C_INT_STOP))
    {
      _err("ERROR: I2C%d unexpected irq in STOP (IPD=0x%08x)\n",
           priv->bus, rk3506_i2c_getreg(priv, RK3506_I2C_REG_IPD));
      rk3506_i2c_clean_ipd(priv);
      return rk3506_i2c_request_stop(priv, -EIO, true);
    }

  /* Ack interrupt and disable stop bit */

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_STOP);

  con = rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON);
  con &= ~RK3506_I2C_CON_STOP;
  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON, con);

  priv->state = RK3506_I2C_STATE_IDLE;
  return RK3506_I2C_XFER_DONE;
}

/****************************************************************************
 * Name: rk3506_i2c_irq_step
 *
 * Description:
 *   Advance the transfer state machine by one interrupt event.
 *
 *   Port of HAL_I2C_IRQHandler() (hal_i2c.c L563-627).  Interrupts stay
 *   masked at the controller (IEN=0); the poll loop calls this directly.
 *
 * Returned Value:
 *   RK3506_I2C_XFER_BUSY - more events needed;
 *   RK3506_I2C_XFER_DONE - segment finished, inspect priv->error.
 *
 ****************************************************************************/

static int rk3506_i2c_irq_step(struct rk3506_i2c_dev_s *priv, int mode,
                               bool islast)
{
  uint32_t ipd = rk3506_i2c_getreg(priv, RK3506_I2C_REG_IPD);

  if (priv->state == RK3506_I2C_STATE_IDLE)
    {
      _err("ERROR: I2C%d irq in STATE_IDLE, IPD=0x%08x\n",
           priv->bus, ipd);
      rk3506_i2c_clean_ipd(priv);
      priv->error = -EIO;
      return RK3506_I2C_XFER_DONE;
    }

  /* Clean interrupt bits we don't care about */

  ipd &= ~(RK3506_I2C_INT_BTF | RK3506_I2C_INT_BRF);

  if (ipd & RK3506_I2C_INT_NAKRCV)
    {
      /* We got a NACK in the last operation.  IGNORE_NAK is never used by
       * this driver (ACTACK is armed in rk3506_i2c_start), so finish the
       * transfer with an error after the STOP has been handled
       * (hal_i2c.c L579-599).
       */

      rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_NAKRCV);
      ipd &= ~RK3506_I2C_INT_NAKRCV;

      if (priv->clkrate == I2C_SPEED_STANDARD)
        {
          up_udelay(1);   /* hal_i2c.c: HAL_DelayUs(1) at 100 kHz */
        }

      return rk3506_i2c_request_stop(priv, -ENXIO, islast);
    }

  /* Is there anything left to handle? */

  if ((ipd & RK3506_I2C_INT_ALL) == 0)
    {
      return RK3506_I2C_XFER_BUSY;
    }

  switch (priv->state)
    {
      case RK3506_I2C_STATE_START:
        return rk3506_i2c_handle_start(priv, mode);

      case RK3506_I2C_STATE_WRITE:
        return rk3506_i2c_handle_write(priv, islast);

      case RK3506_I2C_STATE_READ:
        return rk3506_i2c_handle_read(priv, islast);

      case RK3506_I2C_STATE_STOP:
        return rk3506_i2c_handle_stop(priv);

      default:
        rk3506_i2c_clean_ipd(priv);
        priv->error = -EIO;
        return RK3506_I2C_XFER_DONE;
    }
}

/****************************************************************************
 * Name: rk3506_i2c_start
 *
 * Description:
 *   Enable the controller in the given mode and send START.
 *
 *   Port of I2C_Start() (hal_i2c.c L182-204).  CON[15:8] tuning bits are
 *   preserved; ACTACK makes the controller stop on NACK.
 *
 ****************************************************************************/

static void rk3506_i2c_start(struct rk3506_i2c_dev_s *priv, int mode)
{
  uint32_t val = rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON) &
                 RK3506_I2C_CON_TUNING_MASK;

  rk3506_i2c_clean_ipd(priv);

  priv->state = RK3506_I2C_STATE_START;

  /* Enable adapter with correct mode, send START condition */

  val |= RK3506_I2C_CON_EN;
  val |= ((uint32_t)mode << 1) & RK3506_I2C_CON_MOD_MASK;
  val |= RK3506_I2C_CON_START;

  /* We want to react to NACK */

  val |= RK3506_I2C_CON_ACTACK;

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON, val);
}

/****************************************************************************
 * Name: rk3506_i2c_force_stop
 *
 * Description:
 *   Force a STOP condition without waiting for an interrupt and bring the
 *   engine to IDLE (hal_i2c.c HAL_I2C_ForceStop L707-719).  Used for bus
 *   recovery on timeout.
 *
 ****************************************************************************/

static void rk3506_i2c_force_stop(struct rk3506_i2c_dev_s *priv)
{
  /* Force a STOP condition without interrupt */

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IEN, 0);
  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON,
                    (rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON) &
                     ~RK3506_I2C_CON_TUNING_MASK) |
                    RK3506_I2C_CON_EN | RK3506_I2C_CON_STOP);

  priv->state = RK3506_I2C_STATE_IDLE;
}

/****************************************************************************
 * Name: rk3506_i2c_disable
 *
 * Description:
 *   Disable the controller, keeping the CON[15:8] timing tuning bits
 *   (hal_i2c.c I2C_Disable L167-174; called by HAL_I2C_Close L727-735).
 *
 ****************************************************************************/

static void rk3506_i2c_disable(struct rk3506_i2c_dev_s *priv)
{
  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IEN, 0);
  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON,
                    rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON) &
                    RK3506_I2C_CON_TUNING_MASK);
}

/****************************************************************************
 * Name: rk3506_i2c_prepare_segment
 *
 * Description:
 *   Program slave address / mode for the next hardware segment and pick the
 *   message to transfer.
 *
 *   Port of rockchip_i2c_configure() (drv_i2c.c L99-145, same logic as
 *   Linux rk3x_i2c_setup() i2c-rk3x.c L1109-1180):
 *
 *   - A write of 1..3 register-address bytes immediately followed by a
 *     read is fused into one MOD_REG_TX transaction: MRXADDR holds the
 *     slave address (write polarity) with its valid bit, MRXRADDR holds
 *     the register-address bytes with one valid bit per byte; the hardware
 *     emits write-subaddr, repeated START and read by itself.
 *   - A plain read uses MOD_RX with MRXADDR = slave address with read bit
 *     and valid bit, MRXRADDR invalid.
 *   - A plain write uses MOD_TX; the slave address is the first byte of
 *     the TX FIFO (see rk3506_i2c_fill_tx).
 *
 * Returned Value:
 *   Number of i2c_msg entries consumed (1 or 2), or a negated errno.
 *
 ****************************************************************************/

static int rk3506_i2c_prepare_segment(struct rk3506_i2c_dev_s *priv,
                                      FAR struct i2c_msg_s *msgs,
                                      int remaining, FAR int *mode)
{
  uint32_t addr = (msgs[0].addr & 0x7f) << 1;
  int consumed;

  priv->processed = 0;
  priv->error = 0;
  priv->busy = true;

  if (remaining >= 2 && msgs[0].length < 4 &&
      !(msgs[0].flags & I2C_M_READ) && (msgs[1].flags & I2C_M_READ))
    {
      /* Small write packet (register address) before a read: fuse both
       * messages into one MOD_REG_TX hardware transaction
       * (drv_i2c.c L106-123, i2c-rk3x.c L1121-1143).
       */

      uint32_t regaddr = 0;
      int i;

      for (i = 0; i < msgs[0].length; ++i)
        {
          regaddr |= (uint32_t)msgs[0].buffer[i] << (i * 8);
          regaddr |= RK3506_I2C_MRXADDR_VALID(i);
        }

      /* msgs[0] is handled by hardware.  No read bit on MRXADDR in
       * MOD_REG_TX mode: the controller emits the write/sub-address phase
       * and the repeated-start read phase by itself.
       */

      rk3506_i2c_putreg(priv, RK3506_I2C_REG_MRXADDR,
                        addr | RK3506_I2C_MRXADDR_VALID(0));
      rk3506_i2c_putreg(priv, RK3506_I2C_REG_MRXRADDR, regaddr);

      *mode = RK3506_I2C_MODE_REG_TX;
      priv->msg = &msgs[1];
      consumed = 2;
    }
  else if (msgs[0].flags & I2C_M_READ)
    {
      /* Plain read: master receive mode, slave address with read bit and
       * valid bit in MRXADDR (rk3506.h I2C_MRXADDR L3346-3355).
       *
       * TODO: needs hardware validation.  The SDK RTOS BSP
       * (drv_i2c.c L126-131) and Linux (i2c-rk3x.c L1150-1160) program
       * MOD_REG_TX with MRXRADDR=0 for a plain read; here MOD_RX is used
       * per the eI2C_Mode definition ("receive data") and the mode the
       * HAL itself switches continuation chunks to (hal_i2c.c L273-277).
       * Both paths agree on MRXADDR/MRXCNT; only CON[2:1] differs for the
       * first chunk.  No I2C device on the bench to confirm.
       */

      rk3506_i2c_putreg(priv, RK3506_I2C_REG_MRXADDR,
                        addr | 1 | RK3506_I2C_MRXADDR_VALID(0));
      rk3506_i2c_putreg(priv, RK3506_I2C_REG_MRXRADDR, 0);

      *mode = RK3506_I2C_MODE_RX;
      priv->msg = &msgs[0];
      consumed = 1;
    }
  else
    {
      *mode = RK3506_I2C_MODE_TX;
      priv->msg = &msgs[0];
      consumed = 1;
    }

  return consumed;
}

/****************************************************************************
 * Name: rk3506_i2c_run_segment
 *
 * Description:
 *   Start one hardware segment and drive it to completion by polling IPD.
 *
 *   Same structure as the I2C_POLL loop in drv_i2c.c L197-214: every pass
 *   through HAL_I2C_IRQHandler() (here: rk3506_i2c_irq_step) is a short
 *   spin_lock_irqsave() critical section; the bounded wait between events
 *   happens with interrupts ENABLED via up_udelay() so other interrupts are
 *   not blocked for the whole transfer.
 *
 ****************************************************************************/

static int rk3506_i2c_run_segment(struct rk3506_i2c_dev_s *priv, int mode,
                                  bool islast)
{
  /* 10 us per poll -> RK3506_I2C_TIMEOUT_MS bounded total wait */

  unsigned int waits = RK3506_I2C_TIMEOUT_MS * 100;
  irqstate_t flags;
  int ret;

  flags = spin_lock_irqsave(&priv->lock);
  rk3506_i2c_start(priv, mode);
  spin_unlock_irqrestore(&priv->lock, flags);

  for (; ; )
    {
      /* Short critical section: one state-machine step is a handful of
       * register accesses (at most 8 FIFO words).
       */

      flags = spin_lock_irqsave(&priv->lock);
      ret = rk3506_i2c_irq_step(priv, mode, islast);
      spin_unlock_irqrestore(&priv->lock, flags);

      if (ret != RK3506_I2C_XFER_BUSY)
        {
          break;
        }

      if (waits-- == 0)
        {
          _err("ERROR: I2C%d transfer timeout (mode=%d, state=%d)\n",
               priv->bus, mode, priv->state);

          /* Force STOP to recover the bus (hal_i2c.c HAL_I2C_ForceStop) */

          flags = spin_lock_irqsave(&priv->lock);
          rk3506_i2c_force_stop(priv);
          spin_unlock_irqrestore(&priv->lock, flags);

          priv->busy = false;
          return -ETIMEDOUT;
        }

      /* Wait OUTSIDE the irq-save critical section */

      up_udelay(10);
    }

  if (ret < 0)
    {
      priv->busy = false;
      return ret;
    }

  return priv->error;
}

/****************************************************************************
 * Name: rk3506_i2c_xfer_polling
 *
 * Description:
 *   Execute a list of i2c_msg messages in polling mode (GT911 uses the
 *   polling transfer path).  Mirrors the segment loop in
 *   drv_i2c.c rockchip_i2c_xfer() L170-224.
 *
 ****************************************************************************/

static int rk3506_i2c_xfer_polling(struct rk3506_i2c_dev_s *priv,
                                   FAR struct i2c_msg_s *msgs, int count)
{
  int i = 0;
  int ret = OK;
  irqstate_t flags;

  while (i < count)
    {
      int mode;
      int consumed;
      bool islast;

      flags = spin_lock_irqsave(&priv->lock);
      consumed = rk3506_i2c_prepare_segment(priv, &msgs[i], count - i,
                                            &mode);
      spin_unlock_irqrestore(&priv->lock, flags);

      if (consumed < 0)
        {
          ret = consumed;
          break;
        }

      islast = (i + consumed >= count);

      ret = rk3506_i2c_run_segment(priv, mode, islast);
      if (ret < 0)
        {
          break;
        }

      i += consumed;
    }

  /* HAL_I2C_Close() equivalent: interrupts off and controller disabled so
   * CON.EN is clear before the next transfer (drv_i2c.c L224).
   */

  flags = spin_lock_irqsave(&priv->lock);
  priv->busy = false;
  rk3506_i2c_disable(priv);
  spin_unlock_irqrestore(&priv->lock, flags);

  return ret;
}

/****************************************************************************
 * Name: rk3506_i2c_transfer
 *
 * Description:
 *   Perform one or more I2C transfers (NuttX upper-half interface).
 *
 ****************************************************************************/

static int rk3506_i2c_transfer(FAR struct i2c_master_s *dev,
                               FAR struct i2c_msg_s *msgs, int count)
{
  struct rk3506_i2c_dev_s *priv = (struct rk3506_i2c_dev_s *)dev;
  int ret;

  if (count < 1 || msgs == NULL)
    {
      return -EINVAL;
    }

  /* Bounded idle check with interrupts enabled; the controller ends every
   * previous transfer disabled by rk3506_i2c_xfer_polling().
   */

  ret = rk3506_i2c_wait_bus_not_busy(priv);
  if (ret < 0)
    {
      return ret;
    }

  return rk3506_i2c_xfer_polling(priv, msgs, count);
}

/****************************************************************************
 * Name: rk3506_i2c_reset
 *
 * Description:
 *   Reset the I2C controller (NuttX upper-half interface).
 *
 ****************************************************************************/

#ifdef CONFIG_I2C_RESET
static int rk3506_i2c_reset(FAR struct i2c_master_s *dev)
{
  struct rk3506_i2c_dev_s *priv = (struct rk3506_i2c_dev_s *)dev;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->lock);

  /* Disable controller */

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON, 0);

  /* Clear all interrupts */

  rk3506_i2c_clean_ipd(priv);

  /* Reset clock to default */

  rk3506_i2c_set_clk(priv, I2C_SPEED_DEFAULT);

  priv->state = RK3506_I2C_STATE_IDLE;
  priv->busy  = false;
  priv->error = 0;

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}
#endif

/****************************************************************************
 * Name: rk3506_i2c_initialize
 *
 * Description:
 *   Initialize one I2C bus controller.
 *
 ****************************************************************************/

static int rk3506_i2c_initialize(struct rk3506_i2c_dev_s *priv)
{
  /* Disable controller */

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON, 0);

  /* Clear all interrupts */

  rk3506_i2c_clean_ipd(priv);

  /* Disable all IRQs at controller level */

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IEN, 0);

  /* Set default speed (100 kHz) */

  rk3506_i2c_set_clk(priv, I2C_SPEED_DEFAULT);

  priv->state = RK3506_I2C_STATE_IDLE;
  priv->busy  = false;
  priv->error = 0;

  i2cinfo("I2C%d: initialized (base=0x%08lx, irq=%d)\n",
          priv->bus, (unsigned long)priv->base, priv->irq);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_i2cbus_initialize
 *
 * Description:
 *   Initialize the specified I2C bus and return an I2C master device.
 *
 *   This is called by the board layer (hd_rk3506_i2c.c) to register each
 *   I2C bus with the NuttX I2C framework.
 *
 ****************************************************************************/

FAR struct i2c_master_s *rk3506_i2cbus_initialize(int bus)
{
  struct rk3506_i2c_dev_s *priv;
  static bool initialized = false;

  if (bus < 0 || bus >= RK3506_I2C_NUM_BUS)
    {
      _err("ERROR: Invalid I2C bus number: %d\n", bus);
      return NULL;
    }

  priv = &g_i2c_dev[bus];

  if (!initialized)
    {
      /* One-time global initialization */

      for (int i = 0; i < RK3506_I2C_NUM_BUS; i++)
        {
          g_i2c_dev[i].base    = g_i2c_base[i];
          g_i2c_dev[i].irq     = g_i2c_irq[i];
          g_i2c_dev[i].bus     = i;
          g_i2c_dev[i].state   = RK3506_I2C_STATE_IDLE;
          g_i2c_dev[i].busy    = false;
          g_i2c_dev[i].error   = 0;
          g_i2c_dev[i].clkrate = 0;
          spin_lock_init(&g_i2c_dev[i].lock);
        }

      initialized = true;
    }

  if (priv->dev.ops == NULL)
    {
      static const struct i2c_ops_s rk3506_i2c_ops =
      {
        .transfer = rk3506_i2c_transfer,
#ifdef CONFIG_I2C_RESET
        .reset    = rk3506_i2c_reset
#endif
      };

      priv->dev.ops = &rk3506_i2c_ops;

      /* Ungate this controller's PCLK + CLK_I2Cx before touching its
       * registers (the driver owns its clocks; it no longer relies on
       * U-Boot having ungated them).
       */

      rk3506_i2c_clock_init((uint8_t)bus);

      rk3506_i2c_initialize(priv);
    }

  return &priv->dev;
}
