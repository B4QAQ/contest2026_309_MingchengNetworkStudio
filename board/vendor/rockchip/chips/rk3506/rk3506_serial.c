/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_serial.c
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
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#ifdef CONFIG_SERIAL_TERMIOS
#  include <termios.h>
#endif

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/spinlock.h>
#include <nuttx/init.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/semaphore.h>
#include <nuttx/serial/serial.h>

#include "arm_internal.h"
#include "chip.h"
#include "rk3506_serial.h"
#include "hardware/rk3506_memorymap.h"

#ifdef USE_SERIALDRIVER

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* UART0 Settings - default to U-Boot configuration */

#ifndef CONFIG_UART0_BAUD
#  define CONFIG_UART0_BAUD 115200
#endif

#ifndef CONFIG_UART0_BITS
#  define CONFIG_UART0_BITS 8
#endif

#ifndef CONFIG_UART0_PARITY
#  define CONFIG_UART0_PARITY 0
#endif

#ifndef CONFIG_UART0_2STOP
#  define CONFIG_UART0_2STOP 0
#endif

#ifndef CONFIG_UART0_RXBUFSIZE
#  define CONFIG_UART0_RXBUFSIZE 256
#endif

#ifndef CONFIG_UART0_TXBUFSIZE
#  define CONFIG_UART0_TXBUFSIZE 256
#endif

#ifndef CONFIG_UART1_BAUD
#  define CONFIG_UART1_BAUD 115200
#endif

#ifndef CONFIG_UART1_BITS
#  define CONFIG_UART1_BITS 8
#endif

#ifndef CONFIG_UART1_PARITY
#  define CONFIG_UART1_PARITY 0
#endif

#ifndef CONFIG_UART1_2STOP
#  define CONFIG_UART1_2STOP 0
#endif

#ifndef CONFIG_UART1_RXBUFSIZE
#  define CONFIG_UART1_RXBUFSIZE 256
#endif

#ifndef CONFIG_UART1_TXBUFSIZE
#  define CONFIG_UART1_TXBUFSIZE 256
#endif

/* UART clock frequency
 *
 * RK3506 UART source clock is 24 MHz (XIN24M) by default.
 * The BootROM/MiniLoader does not change the UART clock before loading
 * NuttX (verified by U-Boot's empty board_debug_uart_init()).
 *
 * If you need a different UART clock, configure it in the CRU at boot
 * time before any UART access.
 */

#define UART_SCLK 24000000

/* Timeout for UART busy wait, in milliseconds */

#define UART_TIMEOUT_MS 100

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* RK3506 UART device private data */

struct rk3506_uart_s
{
  uint32_t uartbase;   /* UART base address */
  uint32_t baud;       /* Baud rate */
  uint32_t ier;        /* Saved IER value */
  uint8_t  irq;        /* IRQ number */
  uint8_t  parity;     /* 0=none, 1=odd, 2=even */
  uint8_t  bits;       /* Number of bits (7 or 8) */
  bool     stopbits2;  /* true: 2 stop bits */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  rk3506_uart_setup(struct uart_dev_s *dev);
static void rk3506_uart_shutdown(struct uart_dev_s *dev);
static int  rk3506_uart_attach(struct uart_dev_s *dev);
static void rk3506_uart_detach(struct uart_dev_s *dev);
static int  rk3506_uart_ioctl(struct file *filep, int cmd,
                              unsigned long arg);
static int  rk3506_uart_receive(struct uart_dev_s *dev,
                                unsigned int *status);
static void rk3506_uart_rxint(struct uart_dev_s *dev, bool enable);
static bool rk3506_uart_rxavailable(struct uart_dev_s *dev);
static void rk3506_uart_send(struct uart_dev_s *dev, int ch);
static void rk3506_uart_txint(struct uart_dev_s *dev, bool enable);
static bool rk3506_uart_txready(struct uart_dev_s *dev);
static bool rk3506_uart_txempty(struct uart_dev_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct uart_ops_s g_rk3506_uart_ops =
{
  .setup       = rk3506_uart_setup,
  .shutdown    = rk3506_uart_shutdown,
  .attach      = rk3506_uart_attach,
  .detach      = rk3506_uart_detach,
  .ioctl       = rk3506_uart_ioctl,
  .receive     = rk3506_uart_receive,
  .rxint       = rk3506_uart_rxint,
  .rxavailable = rk3506_uart_rxavailable,
  .send        = rk3506_uart_send,
  .txint       = rk3506_uart_txint,
  .txready     = rk3506_uart_txready,
  .txempty     = rk3506_uart_txempty,
};

/* UART0 */

#ifdef CONFIG_RK3506_UART0
static char g_uart0rxbuffer[CONFIG_UART0_RXBUFSIZE];
static char g_uart0txbuffer[CONFIG_UART0_TXBUFSIZE];

static struct rk3506_uart_s g_uart0priv =
{
  .uartbase  = RK3506_UART0_ADDR,
  .baud      = CONFIG_UART0_BAUD,
  .irq       = RK3506_IRQ_UART0,
  .parity    = CONFIG_UART0_PARITY,
  .bits      = CONFIG_UART0_BITS,
  .stopbits2 = CONFIG_UART0_2STOP,
};

static uart_dev_t g_uart0port =
{
  .recv =
  {
    .size   = CONFIG_UART0_RXBUFSIZE,
    .buffer = g_uart0rxbuffer,
  },
  .xmit =
  {
    .size   = CONFIG_UART0_TXBUFSIZE,
    .buffer = g_uart0txbuffer,
  },
  .ops  = &g_rk3506_uart_ops,
  .priv = &g_uart0priv,
};
#endif

/* UART1 */

#ifdef CONFIG_RK3506_UART1
static char g_uart1rxbuffer[CONFIG_UART1_RXBUFSIZE];
static char g_uart1txbuffer[CONFIG_UART1_TXBUFSIZE];

static struct rk3506_uart_s g_uart1priv =
{
  .uartbase  = RK3506_UART1_ADDR,
  .baud      = CONFIG_UART1_BAUD,
  .irq       = RK3506_IRQ_UART1,
  .parity    = CONFIG_UART1_PARITY,
  .bits      = CONFIG_UART1_BITS,
  .stopbits2 = CONFIG_UART1_2STOP,
};

static uart_dev_t g_uart1port =
{
  .recv =
  {
    .size   = CONFIG_UART1_RXBUFSIZE,
    .buffer = g_uart1rxbuffer,
  },
  .xmit =
  {
    .size   = CONFIG_UART1_TXBUFSIZE,
    .buffer = g_uart1txbuffer,
  },
  .ops  = &g_rk3506_uart_ops,
  .priv = &g_uart1priv,
};
#endif

/* Console device selection */

#ifdef CONFIG_RK3506_UART0
#  define CONSOLE_DEV   g_uart0port
#  define TTYS0_DEV     g_uart0port
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_uart_divisor
 *
 * Description:
 *   Select a divisor to produce the BAUD from the UART SCLK.
 *   BAUD = SCLK / (16 * DL), or
 *   DL   = SCLK / BAUD / 16
 *
 ****************************************************************************/

static uint32_t rk3506_uart_divisor(uint32_t baud)
{
  DEBUGASSERT(baud != 0);
  return UART_SCLK / (baud << 4);
}

/****************************************************************************
 * Name: rk3506_uart_irq_handler
 *
 * Description:
 *   This is the common UART interrupt handler.
 *
 ****************************************************************************/

static int rk3506_uart_irq_handler(int irq, void *context, void *arg)
{
  struct uart_dev_s *dev = (struct uart_dev_s *)arg;
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;
  uint32_t status;
  int passes;

  DEBUGASSERT(dev != NULL && dev->priv != NULL);

  for (passes = 0; passes < 256; passes++)
    {
      status = getreg32(priv->uartbase + RK3506_UART_IIR_OFFSET);

      switch (status & RK3506_UART_IIR_IID_MASK)
        {
          case RK3506_UART_IIR_IID_RECV:
          case RK3506_UART_IIR_IID_TIMEOUT:
            {
              uart_recvchars(dev);
              break;
            }

          case RK3506_UART_IIR_IID_TXEMPTY:
            {
              uart_xmitchars(dev);
              break;
            }

          case RK3506_UART_IIR_IID_NONE:
            {
              return OK;
            }

          default:
            {
              break;
            }
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3506_uart_setup
 *
 * Description:
 *   Configure the UART baud, bits, parity, fifos, etc.
 *
 ****************************************************************************/

static int rk3506_uart_setup(struct uart_dev_s *dev)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;
  uint32_t uartbase;
  uint32_t dl;
  uint32_t lcr;

  DEBUGASSERT(priv != NULL);
  uartbase = priv->uartbase;

  /* Calculate divisor */

  dl = rk3506_uart_divisor(priv->baud);

  /* Build LCR value: data bits, parity, stop bits */

  lcr = 0;
  switch (priv->bits)
    {
      case 5:
        break;
      case 6:
        lcr |= (1 << 0);
        break;
      case 7:
        lcr |= (2 << 0);
        break;
      case 8:
      default:
        lcr |= (3 << 0);
        break;
    }

  if (priv->stopbits2)
    {
      lcr |= (1 << 2);
    }

  if (priv->parity == 1)
    {
      lcr |= (1 << 3);       /* Odd parity */
    }
  else if (priv->parity == 2)
    {
      lcr |= (1 << 3) | (1 << 4); /* Even parity */
    }

  /* Set DLAB to access divisor latch registers */

  putreg32(lcr | RK3506_UART_LCR_DLAB, uartbase + RK3506_UART_LCR_OFFSET);

  /* Set divisor latch low and high bytes */

  putreg32(dl & 0xff, uartbase + RK3506_UART_DLL_OFFSET);
  putreg32((dl >> 8) & 0xff, uartbase + RK3506_UART_DLH_OFFSET);

  /* Clear DLAB and set line control */

  putreg32(lcr, uartbase + RK3506_UART_LCR_OFFSET);

  /* Enable and reset FIFOs */

  putreg32(RK3506_UART_FCR_FIFOE | RK3506_UART_FCR_RFIFOR |
           RK3506_UART_FCR_XFIFOR,
           uartbase + RK3506_UART_FCR_OFFSET);

  /* Save IER - initially disable all interrupts */

  priv->ier = 0;
  putreg32(priv->ier, uartbase + RK3506_UART_IER_OFFSET);

  return OK;
}

/****************************************************************************
 * Name: rk3506_uart_shutdown
 *
 * Description:
 *   Disable the UART.
 *
 ****************************************************************************/

static void rk3506_uart_shutdown(struct uart_dev_s *dev)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;

  DEBUGASSERT(priv != NULL);

  /* Disable all interrupts */

  priv->ier = 0;
  putreg32(0, priv->uartbase + RK3506_UART_IER_OFFSET);
}

/****************************************************************************
 * Name: rk3506_uart_attach
 *
 * Description:
 *   Configure the UART to interrupt on received characters.
 *
 ****************************************************************************/

static int rk3506_uart_attach(struct uart_dev_s *dev)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;
  int ret;

  DEBUGASSERT(priv != NULL);

  ret = irq_attach(priv->irq, rk3506_uart_irq_handler, dev);
  if (ret == OK)
    {
      up_enable_irq(priv->irq);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3506_uart_detach
 *
 * Description:
 *   Detach the UART interrupt.
 *
 ****************************************************************************/

static void rk3506_uart_detach(struct uart_dev_s *dev)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;

  DEBUGASSERT(priv != NULL);

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);
}

/****************************************************************************
 * Name: rk3506_uart_ioctl
 *
 * Description:
 *   ioctl operations on the UART.
 *
 ****************************************************************************/

static int rk3506_uart_ioctl(struct file *filep, int cmd,
                             unsigned long arg)
{
  return -ENOTTY;
}

/****************************************************************************
 * Name: rk3506_uart_receive
 *
 * Description:
 *   Called (usually) from the interrupt handler to receive one character
 *   from the UART.
 *
 ****************************************************************************/

static int rk3506_uart_receive(struct uart_dev_s *dev,
                               unsigned int *status)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;
  uint32_t uartbase;
  uint32_t lsr;

  DEBUGASSERT(priv != NULL);
  uartbase = priv->uartbase;

  lsr = getreg32(uartbase + RK3506_UART_LSR_OFFSET);
  *status = lsr;

  return (int)(getreg32(uartbase + RK3506_UART_RBR_OFFSET) & 0xff);
}

/****************************************************************************
 * Name: rk3506_uart_rxint
 *
 * Description:
 *   Enable or disable RX interrupts.
 *
 ****************************************************************************/

static void rk3506_uart_rxint(struct uart_dev_s *dev, bool enable)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;

  DEBUGASSERT(priv != NULL);

  if (enable)
    {
      priv->ier |= RK3506_UART_IER_ERBFI;
    }
  else
    {
      priv->ier &= ~RK3506_UART_IER_ERBFI;
    }

  putreg32(priv->ier, priv->uartbase + RK3506_UART_IER_OFFSET);
}

/****************************************************************************
 * Name: rk3506_uart_rxavailable
 *
 * Description:
 *   Return true if the receive FIFO is not empty.
 *
 ****************************************************************************/

static bool rk3506_uart_rxavailable(struct uart_dev_s *dev)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;

  DEBUGASSERT(priv != NULL);

  return (getreg32(priv->uartbase + RK3506_UART_LSR_OFFSET) &
          RK3506_UART_LSR_DR) != 0;
}

/****************************************************************************
 * Name: rk3506_uart_send
 *
 * Description:
 *   This method will send one byte on the UART.
 *
 ****************************************************************************/

static void rk3506_uart_send(struct uart_dev_s *dev, int ch)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;

  DEBUGASSERT(priv != NULL);

  putreg32((uint32_t)ch, priv->uartbase + RK3506_UART_THR_OFFSET);
}

/****************************************************************************
 * Name: rk3506_uart_txint
 *
 * Description:
 *   Enable or disable TX interrupts.
 *
 ****************************************************************************/

static void rk3506_uart_txint(struct uart_dev_s *dev, bool enable)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;

  DEBUGASSERT(priv != NULL);

  if (enable)
    {
      priv->ier |= RK3506_UART_IER_ETBEI;
    }
  else
    {
      priv->ier &= ~RK3506_UART_IER_ETBEI;
    }

  putreg32(priv->ier, priv->uartbase + RK3506_UART_IER_OFFSET);
}

/****************************************************************************
 * Name: rk3506_uart_txready
 *
 * Description:
 *   Return true if the transmit FIFO is not full.
 *
 ****************************************************************************/

static bool rk3506_uart_txready(struct uart_dev_s *dev)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;

  DEBUGASSERT(priv != NULL);

  return (getreg32(priv->uartbase + RK3506_UART_LSR_OFFSET) &
          RK3506_UART_LSR_THRE) != 0;
}

/****************************************************************************
 * Name: rk3506_uart_txempty
 *
 * Description:
 *   Return true if the transmit FIFO is empty.
 *
 ****************************************************************************/

static bool rk3506_uart_txempty(struct uart_dev_s *dev)
{
  struct rk3506_uart_s *priv = (struct rk3506_uart_s *)dev->priv;

  DEBUGASSERT(priv != NULL);

  return (getreg32(priv->uartbase + RK3506_UART_LSR_OFFSET) &
          RK3506_UART_LSR_TEMT) != 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_earlyserialinit
 *
 * Description:
 *   Performs the low level UART initialization early in debug so that the
 *   serial console will be available during bootup.
 *
 ****************************************************************************/

void arm_earlyserialinit(void)
{
  /* Enable the console UART */

#ifdef CONSOLE_DEV
  CONSOLE_DEV.isconsole = true;
  rk3506_uart_setup(&CONSOLE_DEV);
#endif
}

/****************************************************************************
 * Name: arm_serialinit
 *
 * Description:
 *   Register serial console and serial ports.
 *
 ****************************************************************************/

void arm_serialinit(void)
{
#ifdef CONSOLE_DEV
  uart_register("/dev/console", &CONSOLE_DEV);
#endif
#ifdef CONFIG_RK3506_UART0
  uart_register("/dev/ttyS0", &g_uart0port);
#endif
#ifdef CONFIG_RK3506_UART1
  uart_register("/dev/ttyS1", &g_uart1port);
#endif
}

/****************************************************************************
 * Name: up_putc
 *
 * Description:
 *   Provide priority, low-level access to support OS debug writes
 *
 ****************************************************************************/

void up_putc(int ch)
{
#ifdef CONSOLE_DEV
  struct rk3506_uart_s *priv =
    (struct rk3506_uart_s *)CONSOLE_DEV.priv;

  /* Wait for the transmit holding register to be empty */

  while (!(getreg32(priv->uartbase + RK3506_UART_LSR_OFFSET) &
           RK3506_UART_LSR_THRE))
    {
    }

  /* Write the character to the transmit holding register */

  putreg32((uint32_t)ch, priv->uartbase + RK3506_UART_THR_OFFSET);
#endif
}

#endif /* USE_SERIALDRIVER */
