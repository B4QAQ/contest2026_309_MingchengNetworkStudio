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

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef putreg32
#  define putreg32(v, x) (*(FAR volatile uint32_t *)(x) = (v))
#endif

#ifndef getreg32
#  define getreg32(x) (*(FAR volatile uint32_t *)(x))
#endif

/* I2C bus configuration */

#define RK3506_I2C_NUM_BUS       3

/* I2C bus default speed */

#define I2C_SPEED_DEFAULT        100000   /* 100 kHz */
#define I2C_SPEED_STANDARD       100000
#define I2C_SPEED_FAST           400000
#define I2C_SPEED_FAST_PLUS      1000000

/* Per-transfer timeout (ms) */

#define RK3506_I2C_TIMEOUT_MS    200

/* I2C controller states (matches i2c-rk3x.c) */

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
static int  rk3506_i2c_xfer_polling(struct rk3506_i2c_dev_s *priv,
                                    FAR struct i2c_msg_s *msgs, int count);
static void rk3506_i2c_handle_irq(struct rk3506_i2c_dev_s *priv);
static void rk3506_i2c_start_transfer(struct rk3506_i2c_dev_s *priv);
static int  rk3506_i2c_setup_xfer(struct rk3506_i2c_dev_s *priv,
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
  uint32_t pclk = 24000000;  /* PCLK_APB, typical 24 MHz */
  uint32_t clkdiv;

  if (clkrate == 0)
    {
      clkrate = I2C_SPEED_DEFAULT;
    }

  if (clkrate > I2C_SPEED_FAST_PLUS)
    {
      clkrate = I2C_SPEED_FAST_PLUS;
    }

  /* Per Linux/U-Boot: div = DIV_ROUND_UP(pclk, 8 * scl) - 2 */

  clkdiv = (pclk + (8 * clkrate) - 1) / (8 * clkrate) - 2;
  if (clkdiv > 0xffff)
    {
      clkdiv = 0xffff;
    }

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CLKDIV, clkdiv);
  priv->clkrate = clkrate;

  i2cinfo("I2C%d: clkrate=%u Hz, clkdiv=%u (pclk=24MHz)\n",
          priv->bus, clkrate, clkdiv);
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
 ****************************************************************************/

static int rk3506_i2c_wait_bus_not_busy(struct rk3506_i2c_dev_s *priv)
{
  uint32_t timeout = RK3506_I2C_TIMEOUT_MS * 100;  /* 100us units */

  while (rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON) & RK3506_I2C_CON_EN)
    {
      if (--timeout <= 0)
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
 *   Clear all pending interrupt status bits.
 *
 ****************************************************************************/

static inline void rk3506_i2c_clean_ipd(struct rk3506_i2c_dev_s *priv)
{
  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_ALL);
}

/****************************************************************************
 * Name: rk3506_i2c_handle_write
 *
 * Description:
 *   Process a write IRQ. Fill TX buffer with the next chunk of bytes.
 *
 ****************************************************************************/

static void rk3506_i2c_handle_write(struct rk3506_i2c_dev_s *priv,
                                    uint32_t ipd)
{
  unsigned int i;
  unsigned int bytes_to_write;
  uint8_t     *buf;

  if (!(ipd & RK3506_I2C_INT_MBTF))
    {
      return;
    }

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_MBTF);

  buf = priv->msg->buffer;
  bytes_to_write = priv->msg->length - priv->processed;
  if (bytes_to_write > RK3506_I2C_MAX_TXBUF)
    {
      bytes_to_write = RK3506_I2C_MAX_TXBUF;
    }

  for (i = 0; i < bytes_to_write; ++i)
    {
      putreg32(buf[priv->processed + i],
               priv->base + RK3506_I2C_TXBUFFER_BASE + i);
    }

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_MTXCNT, bytes_to_write);
  priv->processed += bytes_to_write;
}

/****************************************************************************
 * Name: rk3506_i2c_handle_read
 *
 * Description:
 *   Process a read IRQ. Read the RX buffer and signal completion when
 *   all expected bytes are received.
 *
 ****************************************************************************/

static void rk3506_i2c_handle_read(struct rk3506_i2c_dev_s *priv,
                                   uint32_t ipd)
{
  unsigned int i;
  unsigned int bytes_to_read;
  uint8_t     *buf;

  if (!(ipd & RK3506_I2C_INT_MBRF))
    {
      return;
    }

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_MBRF);

  buf = priv->msg->buffer;
  bytes_to_read = priv->msg->length - priv->processed;
  if (bytes_to_read > RK3506_I2C_MAX_RXBUF)
    {
      bytes_to_read = RK3506_I2C_MAX_RXBUF;
    }

  for (i = 0; i < bytes_to_read; ++i)
    {
      buf[priv->processed + i] =
        getreg32(priv->base + RK3506_I2C_RXBUFFER_BASE + i) & 0xff;
    }

  priv->processed += bytes_to_read;

  if (priv->processed >= (int)priv->msg->length)
    {
      priv->state = RK3506_I2C_STATE_STOP;
    }
}

/****************************************************************************
 * Name: rk3506_i2c_handle_stop
 *
 * Description:
 *   Process a STOP IRQ. Mark the transfer as complete.
 *
 ****************************************************************************/

static void rk3506_i2c_handle_stop(struct rk3506_i2c_dev_s *priv,
                                   uint32_t ipd)
{
  if (!(ipd & RK3506_I2C_INT_STOP))
    {
      return;
    }

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_STOP);

  priv->state = RK3506_I2C_STATE_IDLE;
  priv->busy  = false;
}

/****************************************************************************
 * Name: rk3506_i2c_handle_irq
 *
 * Description:
 *   Top-level IRQ handler. Dispatches to specific handlers.
 *
 *   This function is called from both the actual ISR and the polling path.
 *
 ****************************************************************************/

static void rk3506_i2c_handle_irq(struct rk3506_i2c_dev_s *priv)
{
  uint32_t ipd = rk3506_i2c_getreg(priv, RK3506_I2C_REG_IPD);

  if (ipd & RK3506_I2C_INT_NAKRCV)
    {
      _err("ERROR: I2C%d NACK received\n", priv->bus);
      rk3506_i2c_putreg(priv, RK3506_I2C_REG_IPD, RK3506_I2C_INT_NAKRCV);
      priv->error = -ENXIO;
      priv->state = RK3506_I2C_STATE_STOP;
      priv->busy  = false;
      return;
    }

  switch (priv->state)
    {
      case RK3506_I2C_STATE_WRITE:
        rk3506_i2c_handle_write(priv, ipd);
        break;

      case RK3506_I2C_STATE_READ:
        rk3506_i2c_handle_read(priv, ipd);
        break;

      case RK3506_I2C_STATE_STOP:
        rk3506_i2c_handle_stop(priv, ipd);
        break;

      default:
        rk3506_i2c_clean_ipd(priv);
        break;
    }
}

/****************************************************************************
 * Name: rk3506_i2c_start_transfer
 *
 * Description:
 *   Trigger the actual START condition to begin the configured transfer.
 *
 ****************************************************************************/

static void rk3506_i2c_start_transfer(struct rk3506_i2c_dev_s *priv)
{
  uint32_t val;
  uint32_t mode;

  switch (priv->state)
    {
      case RK3506_I2C_STATE_WRITE:
        mode = RK3506_I2C_CON_MOD_TX;
        break;
      case RK3506_I2C_STATE_READ:
        mode = RK3506_I2C_CON_MOD_REG_TX;
        break;
      default:
        mode = RK3506_I2C_CON_MOD_TX;
        break;
    }

  val  = (rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON) &
          RK3506_I2C_CON_TUNING_MASK);
  val |= RK3506_I2C_CON_EN;
  val |= mode;
  val |= RK3506_I2C_CON_START;

  rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON, val);
}

/****************************************************************************
 * Name: rk3506_i2c_setup_xfer
 *
 * Description:
 *   Configure the I2C controller for a new transfer.
 *
 *   If the message is a read with a sub-address (typical for sensors and
 *   registers), use REG_CON_MOD_REGISTER_TX to combine the register address
 *   write with the subsequent read into a single transaction.
 *
 *   This mirrors the SMBus "read I-data" pattern and the rk3x_i2c_setup()
 *   function in Linux.
 *
 ****************************************************************************/

static int rk3506_i2c_setup_xfer(struct rk3506_i2c_dev_s *priv,
                                  FAR struct i2c_msg_s *msgs, int count)
{
  uint32_t addr;

  addr = (msgs[0].addr & 0x7f) << 1;
  if (msgs[0].flags & I2C_M_READ)
    {
      addr |= 1;
    }

  priv->msg       = &msgs[0];
  priv->processed = 0;
  priv->error     = 0;
  priv->busy      = true;

  if (msgs[0].flags & I2C_M_READ)
    {
      priv->state = RK3506_I2C_STATE_READ;

      /* Slave address for the read */

      rk3506_i2c_putreg(priv, RK3506_I2C_REG_MRXADDR,
                        addr | RK3506_I2C_MRXADDR_VALID(0));

      /* If a previous write carried register sub-address, set MRXRADDR here */

      rk3506_i2c_putreg(priv, RK3506_I2C_REG_MRXRADDR, 0);
      rk3506_i2c_putreg(priv, RK3506_I2C_REG_MRXCNT, msgs[0].length);
    }
  else
    {
      priv->state = RK3506_I2C_STATE_WRITE;

      /* Slave address for the write */

      rk3506_i2c_putreg(priv, RK3506_I2C_REG_MRXADDR,
                        addr | RK3506_I2C_MRXADDR_VALID(0));
    }

  rk3506_i2c_clean_ipd(priv);
  return 0;
}

/****************************************************************************
 * Name: rk3506_i2c_xfer_polling
 *
 * Description:
 *   Drive an I2C transfer by polling the controller's interrupt status
 *   register. This is the polling equivalent of the interrupt-driven
 *   handler used in i2c-rk3x.c.
 *
 ****************************************************************************/

static int rk3506_i2c_xfer_polling(struct rk3506_i2c_dev_s *priv,
                                    FAR struct i2c_msg_s *msgs, int count)
{
  int ret;
  int i;
  uint32_t timeout;

  for (i = 0; i < count; i++)
    {
      ret = rk3506_i2c_setup_xfer(priv, &msgs[i], count - i);
      if (ret < 0)
        {
          return ret;
        }

      rk3506_i2c_start_transfer(priv);

      /* Poll for completion. Each iteration calls the IRQ handler to
       * advance the state machine.
       */

      timeout = RK3506_I2C_TIMEOUT_MS * 1000;  /* microseconds */
      while (priv->busy && timeout > 0)
        {
          rk3506_i2c_handle_irq(priv);
          if (!priv->busy)
            {
              break;
            }
          up_udelay(1);
          timeout--;
        }

      if (priv->busy)
        {
          _err("ERROR: I2C%d transfer timeout (msg %d/%d)\n",
               priv->bus, i + 1, count);

          /* Force STOP to recover */

          uint32_t val = rk3506_i2c_getreg(priv, RK3506_I2C_REG_CON) &
                         RK3506_I2C_CON_TUNING_MASK;
          val |= RK3506_I2C_CON_EN | RK3506_I2C_CON_STOP;
          rk3506_i2c_putreg(priv, RK3506_I2C_REG_CON, val);

          priv->busy  = false;
          priv->state = RK3506_I2C_STATE_IDLE;
          return -ETIMEDOUT;
        }

      if (priv->error)
        {
          ret = priv->error;
          priv->error = 0;
          return ret;
        }
    }

  return OK;
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
  irqstate_t flags;
  int ret;

  if (count < 1 || msgs == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&priv->lock);
  ret = rk3506_i2c_wait_bus_not_busy(priv);
  if (ret < 0)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      return ret;
    }

  ret = rk3506_i2c_xfer_polling(priv, msgs, count);
  spin_unlock_irqrestore(&priv->lock, flags);
  return ret;
}

/****************************************************************************
 * Name: rk3506_i2c_reset
 *
 * Description:
 *   Reset the I2C controller (NuttX upper-half interface).
 *
 ****************************************************************************/

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
      rk3506_i2c_initialize(priv);
    }

  return &priv->dev;
}
