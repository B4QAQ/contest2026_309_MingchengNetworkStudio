/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_usbhost.c
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
 * DWC2 USB Host Controller Driver for RK3506
 *
 * This driver is based on the STM32 OTG FS host driver
 * (arch/arm/src/stm32/stm32_otgfshost.c) and adapted for the RK3506 DWC2
 * controller.  The DWC2 register offsets are identical to the STM32 OTG FS
 * controller; we reuse the definitions from hardware/rk3506_dwc2.h.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/param.h>
#include <sys/types.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/clock.h>
#include <nuttx/signal.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/irq.h>
#include <nuttx/nuttx.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbhost.h>
#include <nuttx/usb/usbhost_devaddr.h>
#include <nuttx/usb/usbhost_trace.h>

#include "arm_internal.h"
#include "rk3506_usbhost.h"
#include "hardware/rk3506_dwc2.h"
#include "hardware/rk3506_memorymap.h"

#ifdef CONFIG_RK3506_USBHOST

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DWC2 OTG base address and IRQ */

#define DWC2_OTG0_BASE   RK3506_USBOTG0_ADDR   /* 0xFF740000 */
#define DWC2_OTG0_IRQ    RK3506_IRQ_USBOTG0

/* Shared INNO USB2 PHY and GRF registers.  All of these come from the
 * Linux SDK as the single source of truth:
 *
 *   - phy_sus / clkout_ctl / port tuning:
 *     kernel-6.1/drivers/phy/rockchip/phy-rockchip-inno-usb2.c,
 *     rk3506_phy_cfgs[] and rk3506_usb2phy_tuning()
 *   - clock gates:
 *     kernel-6.1/drivers/clk/rockchip/clk-rk3506.c
 *     (CLK_REF_USBPHY_TOP = CLKGATE_CON(1) bit 4; OTG0 = CON(7) bits
 *     5..7; OTG1 = CON(7) bits 8..10; PCLK_USBPHY = CON(7) bit 11)
 *
 * The RK3506G2 has ONE INNO USB2 PHY with two ports (OTG0 feeds the
 * Type-C flash/device connector, OTG1 feeds the USB-A host port on this
 * board, per vanxoak-hd-rk3506-evm-v1.dtsi dr_mode).
 *
 * GRF writes use the Rockchip half-word write-enable convention:
 * bits [31:16] = write-enable mask, bits [15:0] = value.  The PHY
 * registers at 0xFF2B0000 are plain read-modify-write.
 */

#define RK3506_GRF_SOC_CON24       0x0060  /* OTG0 port phy_sus, bits [8:0] */
#define RK3506_GRF_SOC_CON28       0x0070  /* OTG1 port phy_sus, bits [8:0] */
#define RK3506_GRF_PHY_SUS_MASK    0x1ffu  /* phy_sus field bits [8:0] */

#define RK3506_USBPHY_CLKOUT_CTL   0x041c  /* 480M clk output, bits [7:2]=0x27 */
#define RK3506_USBPHY_OTG0_TUNE    0x0030  /* OTG0 port tuning */
#define RK3506_USBPHY_OTG0_LSSEL   0x0094  /* OTG0 linestate select */
#define RK3506_USBPHY_OTG1_TUNE    0x0430  /* OTG1 (host) port tuning */
#define RK3506_USBPHY_OTG1_LSSEL   0x0494  /* OTG1 linestate select */

#define RK3506_CRU_CLKGATE_CON(n)  (RK3506_CRU_ADDR + (n) * 4 + 0x800u)
#define RK3506_CON1_REF_USBPHY     (1u << 4)   /* CLK_REF_USBPHY_TOP (24M) */
#define RK3506_CON7_OTG0_BITS      (7u << 5)   /* hclk_otg0 + pmu + adp */
#define RK3506_CON7_OTG1_BITS      (0xfu << 8) /* hclk_otg1 + pmu + adp + pclk_usbphy */

/* Hardware capabilities */

#define RK3506_MAX_TX_FIFOS        RK3506_DWC2_NHOST_CHANNELS
#define RK3506_MAX_PACKET_SIZE     64  /* Full speed max packet size */
#define RK3506_EP0_DEF_PACKET_SIZE 8   /* EP0 default packet size */
#define RK3506_EP0_MAX_PACKET_SIZE 64  /* EP0 FS max packet size */
#define RK3506_MAX_PKTCOUNT        256 /* Max packet count */
#define RK3506_RETRY_COUNT         3   /* Number of ctrl transfer retries */

/* FIFO sizes (in 32-bit words) */

#ifndef CONFIG_RK3506_DWC2_RXFIFO_SIZE
#  define CONFIG_RK3506_DWC2_RXFIFO_SIZE 128
#endif

#ifndef CONFIG_RK3506_DWC2_NPTXFIFO_SIZE
#  define CONFIG_RK3506_DWC2_NPTXFIFO_SIZE 96
#endif

#ifndef CONFIG_RK3506_DWC2_PTXFIFO_SIZE
#  define CONFIG_RK3506_DWC2_PTXFIFO_SIZE 96
#endif

#ifndef CONFIG_RK3506_DWC2_DESCSIZE
#  define CONFIG_RK3506_DWC2_DESCSIZE 128
#endif

/* Delays */

#define RK3506_READY_DELAY         200000      /* In loop counts */
#define RK3506_FLUSH_DELAY         200000      /* In loop counts */
#define RK3506_SETUP_DELAY         SEC2TICK(5) /* 5 seconds in system ticks */
#define RK3506_DATANAK_DELAY       SEC2TICK(5) /* 5 seconds in system ticks */

/* Bound for one channel transfer wait.  Two-stage: the wait runs in
 * RK3506_CHAN_POLL_MSEC slices and slides forward while the channel
 * makes ANY interrupt progress (a slow device that keeps NAKing is
 * making progress - killing that wait would abort a healthy retry
 * loop).  If no progress is seen for RK3506_CHAN_PROG_SEC the core
 * state is dumped and -ETIMEDOUT returned so the class layer can
 * recover.  RK3506_CHAN_WAIT_SEC is the absolute hard cap.
 */

#define RK3506_CHAN_WAIT_SEC       30
#define RK3506_CHAN_PROG_SEC       3
#define RK3506_CHAN_POLL_MSEC      500

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* USB host state machine states */

enum rk3506_smstate_e
{
  SMSTATE_DETACHED = 0,  /* Not attached to a device */
  SMSTATE_ATTACHED,      /* Attached to a device */
  SMSTATE_ENUM,          /* Attached, enumerating */
  SMSTATE_CLASS_BOUND,   /* Enumeration complete, class bound */
};

/* Channel halt reason */

enum rk3506_chreason_e
{
  CHREASON_IDLE = 0,     /* Inactive (initial state) */
  CHREASON_FREED,        /* Channel is no longer in use */
  CHREASON_XFRC,         /* Transfer complete */
  CHREASON_NAK,          /* NAK received */
  CHREASON_NYET,         /* NotYet received */
  CHREASON_STALL,        /* Endpoint stalled */
  CHREASON_TXERR,        /* Transfer error received */
  CHREASON_DTERR,        /* Data toggle error received */
  CHREASON_FRMOR,        /* Frame overrun */
  CHREASON_CANCELLED     /* Transfer cancelled */
};

/* Host channel state */

struct rk3506_chan_s
{
  sem_t             waitsem;   /* Channel wait semaphore */
  volatile int      result;    /* The result of the transfer */
  volatile uint8_t  chreason;  /* Channel halt reason */
  uint8_t           chidx;     /* Channel index */
  uint8_t           epno;      /* Device endpoint number (0-127) */
  uint8_t           eptype;    /* See DWC2_EPTYPE_* definitions */
  uint8_t           funcaddr;  /* Device function address */
  uint8_t           speed;     /* Device speed */
  uint8_t           interval;  /* Interrupt/isochronous EP polling interval */
  uint8_t           pid;       /* Data PID */
  uint8_t           npackets;  /* Number of packets (for data toggle) */
  bool              inuse;     /* True: This channel is "in use" */
  volatile bool     indata1;   /* IN data toggle. True: DATA01 */
  volatile bool     outdata1;  /* OUT data toggle.  True: DATA01 */
  bool              in;        /* True: IN endpoint */
  volatile bool     waiter;    /* True: Thread is waiting */
  uint16_t          maxpacket; /* Max packet size */
  uint16_t          buflen;    /* Buffer length (at start of transfer) */
  volatile uint16_t xfrd;      /* Bytes transferred */
  volatile uint16_t inflight;  /* Number of Tx bytes "in-flight" */
  volatile uint32_t progress;  /* ISR activity counter (NAK/ACK/RX events) */
  uint8_t          *buffer;    /* Transfer buffer pointer */
};

/* Control endpoint container (two channels: IN + OUT) */

struct rk3506_ctrlinfo_s
{
  uint8_t           inndx;     /* EP0 IN control channel index */
  uint8_t           outndx;    /* EP0 OUT control channel index */
};

/* Main driver state */

struct rk3506_usbhost_s
{
  /* Common usbhost_driver_s must be first.  The driver hands &priv->drvr
   * to the USB host class through hport->drvr, and the helpers below cast
   * that pointer back with (struct rk3506_usbhost_s *)drvr, so its offset
   * must stay 0 (same pattern as efm32/kinetis/sam upstream drivers).
   */

  struct usbhost_driver_s drvr;

  /* Connection/enumeration interface, handed to usbhost_waiter() through
   * rk3506_usbhost_initialize().  This is a SEPARATE interface struct:
   * usbhost_connection_s (wait/enumerate) must NOT be aliased onto the
   * top of usbhost_driver_s (ep0configure/epalloc) -- the previous layout
   * bug made conn->wait resolve to rk3506_ep0configure and crashed the
   * kernel on the very first CONN_WAIT.
   */

  struct usbhost_connection_s conn;

  /* Root hub port */

  struct usbhost_roothubport_s rhport;

  /* Overall driver status */

  volatile uint8_t  smstate;   /* The state of the USB host state machine */
  uint8_t           chidx;     /* ID of channel waiting for Tx FIFO space */
  volatile bool     connected; /* Connected to device */
  volatile bool     change;    /* Connection change */
  volatile bool     pscwait;   /* True: Thread is waiting for a port event */
  mutex_t           lock;      /* Support mutually exclusive access */
  sem_t             pscsem;    /* Semaphore to wait for a port event */
  struct rk3506_ctrlinfo_s ep0; /* Root hub port EP0 description */

#ifdef CONFIG_USBHOST_HUB
  volatile struct usbhost_hubport_s *hport;
#endif

  struct usbhost_devaddr_s devgen;  /* Address generation data */

  /* The state of each host channel */

  struct rk3506_chan_s chan[RK3506_MAX_TX_FIFOS];

  /* Hardware base address */

  uintptr_t base;
  int irq;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register operations */

static inline uint32_t rk3506_getreg(struct rk3506_usbhost_s *priv,
                                     uint32_t offset);
static inline void rk3506_putreg(struct rk3506_usbhost_s *priv,
                                 uint32_t offset, uint32_t value);
static inline void rk3506_modifyreg(struct rk3506_usbhost_s *priv,
                                    uint32_t offset, uint32_t clrbits,
                                    uint32_t setbits);

/* Byte stream access */

static inline uint16_t rk3506_getle16(const uint8_t *val);

/* Channel management */

static int rk3506_chan_alloc(struct rk3506_usbhost_s *priv);
static inline void rk3506_chan_free(struct rk3506_usbhost_s *priv,
                                    int chidx);
static inline void rk3506_chan_freeall(struct rk3506_usbhost_s *priv);
static void rk3506_chan_configure(struct rk3506_usbhost_s *priv, int chidx);
static void rk3506_chan_halt(struct rk3506_usbhost_s *priv, int chidx,
                             enum rk3506_chreason_e chreason);
static int rk3506_chan_waitsetup(struct rk3506_usbhost_s *priv,
                                struct rk3506_chan_s *chan);
static int rk3506_chan_wait(struct rk3506_usbhost_s *priv,
                            struct rk3506_chan_s *chan);
static void rk3506_chan_wakeup(struct rk3506_usbhost_s *priv,
                               struct rk3506_chan_s *chan);
static int rk3506_ctrlchan_alloc(struct rk3506_usbhost_s *priv,
                                 uint8_t epno, uint8_t funcaddr,
                                 uint8_t speed,
                                 struct rk3506_ctrlinfo_s *ctrlep);
static int rk3506_ctrlep_alloc(struct rk3506_usbhost_s *priv,
                               const struct usbhost_epdesc_s *epdesc,
                               usbhost_ep_t *ep);
static int rk3506_xfrep_alloc(struct rk3506_usbhost_s *priv,
                              const struct usbhost_epdesc_s *epdesc,
                              usbhost_ep_t *ep);

/* Control/data transfer logic */

static void rk3506_transfer_start(struct rk3506_usbhost_s *priv, int chidx);
static int rk3506_ctrl_sendsetup(struct rk3506_usbhost_s *priv,
                                 struct rk3506_ctrlinfo_s *ep0,
                                 const struct usb_ctrlreq_s *req);
static int rk3506_ctrl_senddata(struct rk3506_usbhost_s *priv,
                                struct rk3506_ctrlinfo_s *ep0,
                                uint8_t *buffer, unsigned int buflen);
static int rk3506_ctrl_recvdata(struct rk3506_usbhost_s *priv,
                                struct rk3506_ctrlinfo_s *ep0,
                                uint8_t *buffer, unsigned int buflen);
static int rk3506_in_setup(struct rk3506_usbhost_s *priv, int chidx);
static ssize_t rk3506_in_transfer(struct rk3506_usbhost_s *priv, int chidx,
                                  uint8_t *buffer, size_t buflen);
static int rk3506_out_setup(struct rk3506_usbhost_s *priv, int chidx);
static ssize_t rk3506_out_transfer(struct rk3506_usbhost_s *priv,
                                   int chidx, uint8_t *buffer,
                                   size_t buflen);

/* Interrupt handling */

static void rk3506_gint_wrpacket(struct rk3506_usbhost_s *priv,
                                 uint8_t *buffer, int chidx, int buflen);
static inline void rk3506_gint_hcinisr(struct rk3506_usbhost_s *priv,
                                       int chidx);
static inline void rk3506_gint_hcoutisr(struct rk3506_usbhost_s *priv,
                                        int chidx);
static void rk3506_gint_connected(struct rk3506_usbhost_s *priv);
static void rk3506_gint_disconnected(struct rk3506_usbhost_s *priv);
static inline void rk3506_gint_rxflvlisr(struct rk3506_usbhost_s *priv);
static inline void rk3506_gint_nptxfeisr(struct rk3506_usbhost_s *priv);
static inline void rk3506_gint_ptxfeisr(struct rk3506_usbhost_s *priv);
static inline void rk3506_gint_hcisr(struct rk3506_usbhost_s *priv);
static inline void rk3506_gint_hprtisr(struct rk3506_usbhost_s *priv);
static inline void rk3506_gint_discisr(struct rk3506_usbhost_s *priv);
static inline void rk3506_gint_ipxfrisr(struct rk3506_usbhost_s *priv);
static int rk3506_gint_isr(int irq, void *context, void *arg);

/* Interrupt controls */

static void rk3506_gint_enable(struct rk3506_usbhost_s *priv);
static void rk3506_gint_disable(struct rk3506_usbhost_s *priv);
static inline void rk3506_hostinit_enable(struct rk3506_usbhost_s *priv);
static void rk3506_txfe_enable(struct rk3506_usbhost_s *priv, int chidx);

/* USB host controller operations */

static int rk3506_wait(struct usbhost_connection_s *conn,
                       struct usbhost_hubport_s **hport);
static int rk3506_rh_enumerate(struct rk3506_usbhost_s *priv,
                               struct usbhost_connection_s *conn,
                               struct usbhost_hubport_s *hport);
static int rk3506_enumerate(struct usbhost_connection_s *conn,
                            struct usbhost_hubport_s *hport);
static int rk3506_ep0configure(struct usbhost_driver_s *drvr,
                               usbhost_ep_t ep0, uint8_t funcaddr,
                               uint8_t speed, uint16_t maxpacketsize);
static int rk3506_epalloc(struct usbhost_driver_s *drvr,
                          const struct usbhost_epdesc_s *epdesc,
                          usbhost_ep_t *ep);
static int rk3506_epfree(struct usbhost_driver_s *drvr, usbhost_ep_t ep);
static int rk3506_alloc(struct usbhost_driver_s *drvr,
                        uint8_t **buffer, size_t *maxlen);
static int rk3506_free(struct usbhost_driver_s *drvr, uint8_t *buffer);
static int rk3506_ioalloc(struct usbhost_driver_s *drvr,
                          uint8_t **buffer, size_t buflen);
static int rk3506_iofree(struct usbhost_driver_s *drvr, uint8_t *buffer);
static int rk3506_ctrlin(struct usbhost_driver_s *drvr, usbhost_ep_t ep0,
                         const struct usb_ctrlreq_s *req,
                         uint8_t *buffer);
static int rk3506_ctrlout(struct usbhost_driver_s *drvr, usbhost_ep_t ep0,
                          const struct usb_ctrlreq_s *req,
                          const uint8_t *buffer);
static ssize_t rk3506_transfer(struct usbhost_driver_s *drvr,
                               usbhost_ep_t ep, uint8_t *buffer,
                               size_t buflen);
static int rk3506_cancel(struct usbhost_driver_s *drvr, usbhost_ep_t ep);
#ifdef CONFIG_USBHOST_HUB
static int rk3506_connect(struct usbhost_driver_s *drvr,
                          struct usbhost_hubport_s *hport,
                          bool connected);
#endif
static void rk3506_disconnect(struct usbhost_driver_s *drvr,
                              struct usbhost_hubport_s *hport);

/* Initialization */

static void rk3506_portreset(struct rk3506_usbhost_s *priv);
static void rk3506_flush_txfifos(struct rk3506_usbhost_s *priv,
                                 uint32_t txfnum);
static void rk3506_flush_rxfifo(struct rk3506_usbhost_s *priv);
static void rk3506_vbusdrive(struct rk3506_usbhost_s *priv, bool state);
static void rk3506_host_initialize(struct rk3506_usbhost_s *priv);
static inline void rk3506_sw_initialize(struct rk3506_usbhost_s *priv);
static inline int rk3506_hw_initialize(struct rk3506_usbhost_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Static instances for OTG0 (and optionally OTG1) */

static struct rk3506_usbhost_s g_usbhost0 =
{
  .conn =
  {
    .wait      = rk3506_wait,
    .enumerate = rk3506_enumerate,
  },
  .lock   = NXMUTEX_INITIALIZER,
  .pscsem = SEM_INITIALIZER(0),
};

#ifdef CONFIG_RK3506_USBHOST_OTG1
static struct rk3506_usbhost_s g_usbhost1 =
{
  .conn =
  {
    .wait      = rk3506_wait,
    .enumerate = rk3506_enumerate,
  },
  .lock   = NXMUTEX_INITIALIZER,
  .pscsem = SEM_INITIALIZER(0),
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_getreg, rk3506_putreg, rk3506_modifyreg
 ****************************************************************************/

static inline uint32_t rk3506_getreg(struct rk3506_usbhost_s *priv,
                                     uint32_t offset)
{
  return getreg32(priv->base + offset);
}

static inline void rk3506_putreg(struct rk3506_usbhost_s *priv,
                                 uint32_t offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

static inline void rk3506_modifyreg(struct rk3506_usbhost_s *priv,
                                    uint32_t offset, uint32_t clrbits,
                                    uint32_t setbits)
{
  uint32_t regval = rk3506_getreg(priv, offset);
  regval = (regval & ~clrbits) | setbits;
  rk3506_putreg(priv, offset, regval);
}

/****************************************************************************
 * Name: rk3506_getle16
 ****************************************************************************/

static inline uint16_t rk3506_getle16(const uint8_t *val)
{
  return (uint16_t)val[1] << 8 | (uint16_t)val[0];
}

/****************************************************************************
 * Name: rk3506_chan_alloc
 ****************************************************************************/

static int rk3506_chan_alloc(struct rk3506_usbhost_s *priv)
{
  int chidx;

  for (chidx = 0; chidx < RK3506_DWC2_NHOST_CHANNELS; chidx++)
    {
      if (!priv->chan[chidx].inuse)
        {
          priv->chan[chidx].inuse = true;
          return chidx;
        }
    }

  return -EBUSY;
}

/****************************************************************************
 * Name: rk3506_chan_free
 ****************************************************************************/

static void rk3506_chan_free(struct rk3506_usbhost_s *priv, int chidx)
{
  DEBUGASSERT((unsigned)chidx < RK3506_DWC2_NHOST_CHANNELS);
  rk3506_chan_halt(priv, chidx, CHREASON_FREED);
  priv->chan[chidx].inuse = false;
}

/****************************************************************************
 * Name: rk3506_chan_freeall
 ****************************************************************************/

static inline void rk3506_chan_freeall(struct rk3506_usbhost_s *priv)
{
  uint8_t chidx;

  for (chidx = 2; chidx < RK3506_DWC2_NHOST_CHANNELS; chidx++)
    {
      rk3506_chan_free(priv, chidx);
    }
}

/****************************************************************************
 * Name: rk3506_chan_intmsk
 *
 * Description:
 *   Compute the HCINTMSK value for a channel transfer.  Derived from the
 *   reference DWC2 host drivers (u-boot dwc2.c hc_init): CHH is always
 *   enabled so that ANY channel halt - including self-halts initiated by
 *   the core (NYET, babble, frame overrun) - wakes the waiter.  ACK is
 *   enabled for control/bulk IN: it fires per received data packet and
 *   lets the ISR finish a transfer whose data is complete but whose XFRC
 *   does not arrive (observed on this core after NAK-retry sequences).
 *
 *   The ep0 control channel alternates IN/OUT between transfers, so this
 *   must be recomputed per transfer, not per channel allocation.
 *
 ****************************************************************************/

static uint32_t rk3506_chan_intmsk(FAR struct rk3506_chan_s *chan)
{
  uint32_t regval = 0;

  switch (chan->eptype)
    {
    case DWC2_EPTYPE_CTRL:
    case DWC2_EPTYPE_BULK:
      {
        regval |= (DWC2_HCINT_XFRC  | DWC2_HCINT_STALL | DWC2_HCINT_NAK |
                   DWC2_HCINT_TXERR | DWC2_HCINT_DTERR | DWC2_HCINT_CHH);

        if (chan->in)
          {
            regval |= (DWC2_HCINT_BBERR | DWC2_HCINT_ACK);
          }
        else
          {
            regval |= DWC2_HCINT_NYET;
          }
      }
      break;

    case DWC2_EPTYPE_INTR:
      {
        regval |= (DWC2_HCINT_XFRC | DWC2_HCINT_STALL |
                   DWC2_HCINT_NAK   | DWC2_HCINT_TXERR |
                   DWC2_HCINT_FRMOR | DWC2_HCINT_DTERR |
                   DWC2_HCINT_CHH);

        if (chan->in)
          {
            regval |= DWC2_HCINT_BBERR;
          }
      }
      break;

    case DWC2_EPTYPE_ISOC:
      {
        regval |= (DWC2_HCINT_XFRC | DWC2_HCINT_ACK | DWC2_HCINT_FRMOR |
                   DWC2_HCINT_CHH);

        if (chan->in)
          {
            regval |= (DWC2_HCINT_TXERR | DWC2_HCINT_BBERR);
          }
      }
      break;
    }

  return regval;
}

/****************************************************************************
 * Name: rk3506_chan_configure
 ****************************************************************************/

static void rk3506_chan_configure(struct rk3506_usbhost_s *priv, int chidx)
{
  struct rk3506_chan_s *chan = &priv->chan[chidx];
  uint32_t regval;

  /* Clear any old pending interrupts for this host channel. */

  rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), 0xffffffff);

  /* Enable channel interrupts required for transfers on this channel. */

  regval = rk3506_chan_intmsk(chan);

  rk3506_putreg(priv, DWC2_HCINTMSK_OFFSET(chidx), regval);

  /* Enable the top level host channel interrupt. */

  rk3506_modifyreg(priv, DWC2_HAINTMSK_OFFSET, 0,
                   DWC2_HAINT(chidx));

  /* Make sure host channel interrupts are enabled. */

  rk3506_modifyreg(priv, DWC2_GINTMSK_OFFSET, 0, DWC2_GINT_HC);

  /* Program the HCCHAR register */

  regval = ((uint32_t)chan->maxpacket << DWC2_HCCHAR_MPSIZ_SHIFT) |
           ((uint32_t)chan->epno      << DWC2_HCCHAR_EPNUM_SHIFT) |
           ((uint32_t)chan->eptype    << DWC2_HCCHAR_EPTYP_SHIFT) |
           ((uint32_t)chan->funcaddr  << DWC2_HCCHAR_DAD_SHIFT);

  if (chan->speed == USB_SPEED_LOW)
    {
      regval |= DWC2_HCCHAR_LSDEV;
    }

  if (chan->in)
    {
      regval |= DWC2_HCCHAR_EPDIR_IN;
    }

  if (chan->eptype == DWC2_EPTYPE_INTR)
    {
      regval |= DWC2_HCCHAR_ODDFRM;
    }

  rk3506_putreg(priv, DWC2_HCCHAR_OFFSET(chidx), regval);
}

/****************************************************************************
 * Name: rk3506_chan_halt
 ****************************************************************************/

static void rk3506_chan_halt(struct rk3506_usbhost_s *priv, int chidx,
                             enum rk3506_chreason_e chreason)
{
  uint32_t hcchar;
  uint32_t intmsk;
  uint32_t eptype;
  unsigned int avail;

  priv->chan[chidx].chreason = (uint8_t)chreason;

  hcchar  = rk3506_getreg(priv, DWC2_HCCHAR_OFFSET(chidx));
  hcchar |= (DWC2_HCCHAR_CHDIS | DWC2_HCCHAR_CHENA);

  eptype = hcchar & DWC2_HCCHAR_EPTYP_MASK;

  if (eptype == DWC2_HCCHAR_EPTYP_CTRL ||
      eptype == DWC2_HCCHAR_EPTYP_BULK)
    {
      avail = rk3506_getreg(priv, DWC2_HNPTXSTS_OFFSET) &
              DWC2_HNPTXSTS_NPTXFSAV_MASK;
    }
  else
    {
      avail = rk3506_getreg(priv, DWC2_HPTXSTS_OFFSET) &
              DWC2_HPTXSTS_PTXFSAVL_MASK;
    }

  if (avail == 0)
    {
      hcchar &= ~DWC2_HCCHAR_CHENA;
    }

  intmsk  = rk3506_getreg(priv, DWC2_HCINTMSK_OFFSET(chidx));
  intmsk |= DWC2_HCINT_CHH;
  rk3506_putreg(priv, DWC2_HCINTMSK_OFFSET(chidx), intmsk);

  rk3506_putreg(priv, DWC2_HCCHAR_OFFSET(chidx), hcchar);
}

/****************************************************************************
 * Name: rk3506_chan_waitsetup
 ****************************************************************************/

static int rk3506_chan_waitsetup(struct rk3506_usbhost_s *priv,
                                struct rk3506_chan_s *chan)
{
  irqstate_t flags = enter_critical_section();
  int        ret   = -ENODEV;

  if (priv->connected)
    {
      chan->waiter = true;
      ret         = OK;
    }

  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: rk3506_chan_dump
 *
 * Description:
 *   Dump the core state of a channel that stopped making progress.
 *   Read-only registers only (GRXSTSP is intentionally NOT read - it
 *   would pop a FIFO status entry).
 *
 ****************************************************************************/

static void rk3506_chan_dump(FAR struct rk3506_usbhost_s *priv, int chidx,
                             const char *tag)
{
  FAR struct rk3506_chan_s *chan = &priv->chan[chidx];

  uwarn("WARNING: ch%d %s: transfer stalled, core state:\n", chidx, tag);
  uwarn("  GINTSTS=%08lx GINTMSK=%08lx HAINT=%04lx\n",
        (unsigned long)rk3506_getreg(priv, DWC2_GINTSTS_OFFSET),
        (unsigned long)rk3506_getreg(priv, DWC2_GINTMSK_OFFSET),
        (unsigned long)rk3506_getreg(priv, DWC2_HAINT_OFFSET));
  uwarn("  HCINT=%08lx HCINTMSK=%08lx\n",
        (unsigned long)rk3506_getreg(priv, DWC2_HCINT_OFFSET(chidx)),
        (unsigned long)rk3506_getreg(priv, DWC2_HCINTMSK_OFFSET(chidx)));
  uwarn("  HCCHAR=%08lx HCTSIZ=%08lx\n",
        (unsigned long)rk3506_getreg(priv, DWC2_HCCHAR_OFFSET(chidx)),
        (unsigned long)rk3506_getreg(priv, DWC2_HCTSIZ_OFFSET(chidx)));
  uwarn("  chan: eptype=%u in=%u epno=%u mps=%u pid=%u "
        "buflen=%u xfrd=%u progress=%u\n",
        (unsigned)chan->eptype, (unsigned)chan->in,
        (unsigned)chan->epno, (unsigned)chan->maxpacket,
        (unsigned)chan->pid, (unsigned)chan->buflen,
        (unsigned)chan->xfrd, (unsigned)chan->progress);
}

/****************************************************************************
 * Name: rk3506_chan_wait
 *
 * Description:
 *   Wait for the completion interrupt of the current channel transfer.
 *   Bounded by RK3506_CHAN_WAIT_DELAY: a channel that never completes
 *   and never interrupts (device went silent on the wire) used to
 *   block its caller forever - now the core state is dumped and
 *   -ETIMEDOUT returned so the class layer can recover.
 *
 ****************************************************************************/

static int rk3506_chan_wait(struct rk3506_usbhost_s *priv,
                            struct rk3506_chan_s *chan)
{
  irqstate_t flags;
  struct timespec deadline;
  struct timespec start;
  struct timespec lastprog;
  struct timespec now;
  uint32_t progress;
  int chidx = (int)(chan - priv->chan);
  int ret;

  clock_gettime(CLOCK_MONOTONIC, &start);
  lastprog = start;

  flags = enter_critical_section();

  ret = 0;

  for (; ; )
    {
      clock_gettime(CLOCK_MONOTONIC, &now);
      deadline            = now;
      deadline.tv_nsec   += RK3506_CHAN_POLL_MSEC * 1000000;
      if (deadline.tv_nsec >= 1000000000)
        {
          deadline.tv_sec  += 1;
          deadline.tv_nsec -= 1000000000;
        }

      progress = chan->progress;

      ret = nxsem_timedwait_uninterruptible(&chan->waitsem, &deadline);
      if (ret < 0 && ret != -ETIMEDOUT)
        {
          break;
        }

      if (ret == 0 && (chan->result != EBUSY || !chan->waiter))
        {
          /* Posted with a verdict: transfer completed (or the waiter
           * lost the race with a disconnect). */

          ret = -(int)chan->result;
          break;
        }

      clock_gettime(CLOCK_MONOTONIC, &now);

      if (chan->progress != progress)
        {
          /* Interrupt progress in this slice: refresh the stall
           * window. */

          lastprog = now;
        }

      /* Stalled: no interrupt progress within the window. */

      if (now.tv_sec - lastprog.tv_sec >= RK3506_CHAN_PROG_SEC)
        {
          ret = -ETIMEDOUT;
          break;
        }

      /* Hard cap: never wait longer than this, even with progress. */

      if (now.tv_sec - start.tv_sec >= RK3506_CHAN_WAIT_SEC)
        {
          ret = -ETIMEDOUT;
          break;
        }
    }

  if (ret == -ETIMEDOUT)
    {
      /* The completion may have raced with the timeout: re-check
       * under the critical section before giving up.
       */

      if (!chan->waiter)
        {
          ret = -(int)chan->result;
        }
      else
        {
          chan->waiter = false;
          rk3506_chan_dump(priv, chidx, "timeout");
          rk3506_chan_halt(priv, chidx, CHREASON_CANCELLED);
          ret = -ETIMEDOUT;
        }
    }

  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: rk3506_chan_wakeup
 ****************************************************************************/

static void rk3506_chan_wakeup(struct rk3506_usbhost_s *priv,
                               struct rk3506_chan_s *chan)
{
  if (chan->result != EBUSY)
    {
      if (chan->waiter)
        {
          nxsem_post(&chan->waitsem);
          chan->waiter = false;
        }
    }
}

/****************************************************************************
 * Name: rk3506_ctrlchan_alloc
 ****************************************************************************/

static int rk3506_ctrlchan_alloc(struct rk3506_usbhost_s *priv,
                                 uint8_t epno, uint8_t funcaddr,
                                 uint8_t speed,
                                 struct rk3506_ctrlinfo_s *ctrlep)
{
  struct rk3506_chan_s *chan;
  int inndx;
  int outndx;

  outndx = rk3506_chan_alloc(priv);
  if (outndx < 0)
    {
      return -ENOMEM;
    }

  ctrlep->outndx  = outndx;
  chan            = &priv->chan[outndx];
  chan->epno      = epno;
  chan->in        = false;
  chan->eptype    = DWC2_EPTYPE_CTRL;
  chan->funcaddr  = funcaddr;
  chan->speed     = speed;
  chan->interval  = 0;
  chan->maxpacket = RK3506_EP0_DEF_PACKET_SIZE;
  chan->indata1   = false;
  chan->outdata1  = false;

  rk3506_chan_configure(priv, outndx);

  inndx = rk3506_chan_alloc(priv);
  if (inndx < 0)
    {
      rk3506_chan_free(priv, outndx);
      return -ENOMEM;
    }

  ctrlep->inndx   = inndx;
  chan            = &priv->chan[inndx];
  chan->epno      = epno;
  chan->in        = true;
  chan->eptype    = DWC2_EPTYPE_CTRL;
  chan->funcaddr  = funcaddr;
  chan->speed     = speed;
  chan->interval  = 0;
  chan->maxpacket = RK3506_EP0_DEF_PACKET_SIZE;
  chan->indata1   = false;
  chan->outdata1  = false;

  rk3506_chan_configure(priv, inndx);
  return OK;
}

/****************************************************************************
 * Name: rk3506_ctrlep_alloc
 ****************************************************************************/

static int rk3506_ctrlep_alloc(struct rk3506_usbhost_s *priv,
                               const struct usbhost_epdesc_s *epdesc,
                               usbhost_ep_t *ep)
{
  struct usbhost_hubport_s *hport;
  struct rk3506_ctrlinfo_s *ctrlep;
  int ret;

  DEBUGASSERT(epdesc->hport != NULL);
  hport = epdesc->hport;

  ctrlep = (struct rk3506_ctrlinfo_s *)
    kmm_malloc(sizeof(struct rk3506_ctrlinfo_s));
  if (ctrlep == NULL)
    {
      uerr("ERROR: Failed to allocate control endpoint container\n");
      return -ENOMEM;
    }

  ret = rk3506_ctrlchan_alloc(priv, epdesc->addr & USB_EPNO_MASK,
                              hport->funcaddr, hport->speed, ctrlep);
  if (ret < 0)
    {
      uerr("ERROR: rk3506_ctrlchan_alloc failed: %d\n", ret);
      kmm_free(ctrlep);
      return ret;
    }

  *ep = (usbhost_ep_t)ctrlep;
  return OK;
}

/****************************************************************************
 * Name: rk3506_xfrep_alloc
 ****************************************************************************/

static int rk3506_xfrep_alloc(struct rk3506_usbhost_s *priv,
                              const struct usbhost_epdesc_s *epdesc,
                              usbhost_ep_t *ep)
{
  struct usbhost_hubport_s *hport;
  struct rk3506_chan_s *chan;
  int chidx;

  DEBUGASSERT(epdesc->hport != NULL);
  hport = epdesc->hport;

  chidx = rk3506_chan_alloc(priv);
  if (chidx < 0)
    {
      uerr("ERROR: Failed to allocate a host channel\n");
      return -ENOMEM;
    }

  chan            = &priv->chan[chidx];
  chan->epno      = epdesc->addr & USB_EPNO_MASK;
  chan->in        = epdesc->in;
  chan->eptype    = epdesc->xfrtype;
  chan->funcaddr  = hport->funcaddr;
  chan->speed     = hport->speed;
  chan->interval  = epdesc->interval;
  chan->maxpacket = epdesc->mxpacketsize;
  chan->indata1   = false;
  chan->outdata1  = false;

  rk3506_chan_configure(priv, chidx);

  *ep = (usbhost_ep_t)chidx;
  return OK;
}

/****************************************************************************
 * Name: rk3506_transfer_start
 ****************************************************************************/

static void rk3506_transfer_start(struct rk3506_usbhost_s *priv, int chidx)
{
  struct rk3506_chan_s *chan;
  uint32_t regval;
  unsigned int npackets;
  unsigned int maxpacket;
  unsigned int avail;
  unsigned int wrsize;
  unsigned int minsize;

  chan           = &priv->chan[chidx];

  chan->result   = EBUSY;
  chan->inflight = 0;
  chan->xfrd     = 0;
  priv->chidx    = chidx;

  maxpacket = chan->maxpacket;

  /* Fresh interrupt state for this transfer: clear stale raw status and
   * (re)arm the interrupt mask for this channel/direction combination.
   * The ep0 control channel alternates IN/OUT between transfers and CHH
   * must always be armed, so the mask cannot be left over from the
   * previous transfer. */

  rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), 0xffffffff);
  rk3506_putreg(priv, DWC2_HCINTMSK_OFFSET(chidx),
                rk3506_chan_intmsk(chan));
  rk3506_modifyreg(priv, DWC2_HAINTMSK_OFFSET, 0, DWC2_HAINT(chidx));
  rk3506_modifyreg(priv, DWC2_GINTMSK_OFFSET, 0, DWC2_GINT_HC);

  uinfo("ch%d start: type=%s in=%u ep=%u mps=%u pid=%u len=%u\n",
        chidx,
        chan->eptype == DWC2_EPTYPE_CTRL ? "ctrl" :
        chan->eptype == DWC2_EPTYPE_BULK ? "bulk" :
        chan->eptype == DWC2_EPTYPE_INTR ? "intr" : "isoc",
        (unsigned)chan->in, (unsigned)chan->epno,
        (unsigned)chan->maxpacket, (unsigned)chan->pid,
        (unsigned)chan->buflen);

  if (chan->buflen > maxpacket)
    {
      npackets = (chan->buflen + maxpacket - 1) / maxpacket;

      if (npackets > RK3506_MAX_PKTCOUNT)
        {
          npackets = RK3506_MAX_PKTCOUNT;
          chan->buflen = RK3506_MAX_PKTCOUNT * maxpacket;
        }
    }
  else
    {
      npackets = 1;
    }

  /* Limit npackets to uint8_t range (0-255) to prevent truncation.
   * DWC2 PKTCNT field is 10 bits, but struct npackets is uint8_t.
   */

  if (npackets > 255)
    {
      npackets = 255;
      chan->buflen = 255 * maxpacket;
    }

  chan->npackets = (uint8_t)npackets;

  /* Setup HCTSIZn */

  regval = ((uint32_t)chan->buflen << DWC2_HCTSIZ_XFRSIZ_SHIFT) |
           ((uint32_t)npackets << DWC2_HCTSIZ_PKTCNT_SHIFT) |
           ((uint32_t)chan->pid << DWC2_HCTSIZ_DPID_SHIFT);
  rk3506_putreg(priv, DWC2_HCTSIZ_OFFSET(chidx), regval);

  /* Setup HCCHAR: Frame oddness and channel enable */

  regval = rk3506_getreg(priv, DWC2_HCCHAR_OFFSET(chidx));

  if ((rk3506_getreg(priv, DWC2_HFNUM_OFFSET) & 1) == 0)
    {
      regval |= DWC2_HCCHAR_ODDFRM;
    }
  else
    {
      regval &= ~DWC2_HCCHAR_ODDFRM;
    }

  regval &= ~DWC2_HCCHAR_CHDIS;
  regval |= DWC2_HCCHAR_CHENA;
  rk3506_putreg(priv, DWC2_HCCHAR_OFFSET(chidx), regval);

  /* For OUT transfers, copy data into the TxFIFO */

  if (!chan->in && chan->buflen > 0)
    {
      minsize = MIN(chan->buflen, chan->maxpacket);

      switch (chan->eptype)
        {
        case DWC2_EPTYPE_CTRL:
        case DWC2_EPTYPE_BULK:
          {
            regval = rk3506_getreg(priv, DWC2_HNPTXSTS_OFFSET);
            avail  = ((regval & DWC2_HNPTXSTS_NPTXFSAV_MASK) >>
                      DWC2_HNPTXSTS_NPTXFSAV_SHIFT) << 2;
          }
          break;

        case DWC2_EPTYPE_INTR:
        case DWC2_EPTYPE_ISOC:
          {
            regval = rk3506_getreg(priv, DWC2_HPTXSTS_OFFSET);
            avail  = ((regval & DWC2_HPTXSTS_PTXFSAVL_MASK) >>
                      DWC2_HPTXSTS_PTXFSAVL_SHIFT) << 2;
          }
          break;

        default:
          DEBUGPANIC();
          return;
        }

      if (minsize <= avail)
        {
          wrsize = chan->buflen;
          if (wrsize > avail)
            {
              unsigned int wrpackets = avail / chan->maxpacket;
              wrsize = wrpackets * chan->maxpacket;
            }

          rk3506_gint_wrpacket(priv, chan->buffer, chidx, wrsize);
        }

      if (chan->buflen > avail)
        {
          rk3506_txfe_enable(priv, chidx);
        }
    }
}

/****************************************************************************
 * Name: rk3506_ctrl_sendsetup
 ****************************************************************************/

static int rk3506_ctrl_sendsetup(struct rk3506_usbhost_s *priv,
                                 struct rk3506_ctrlinfo_s *ep0,
                                 const struct usb_ctrlreq_s *req)
{
  struct rk3506_chan_s *chan;
  clock_t start;
  clock_t elapsed;
  int ret;

  chan  = &priv->chan[ep0->outndx];
  start = clock_systime_ticks();

  do
    {
      chan->pid    = DWC2_PID_SETUP;
      chan->buffer = (uint8_t *)req;
      chan->buflen = USB_SIZEOF_CTRLREQ;
      chan->xfrd   = 0;

      ret = rk3506_chan_waitsetup(priv, chan);
      if (ret < 0)
        {
          return ret;
        }

      rk3506_transfer_start(priv, ep0->outndx);

      ret = rk3506_chan_wait(priv, chan);

      if (ret != -EAGAIN)
        {
          if (ret < 0)
            {
              usbhost_trace1(DWC2_TRACE1_SENDSETUP, -ret);
            }

          return ret;
        }

      elapsed = clock_systime_ticks() - start;
    }
  while (elapsed < RK3506_SETUP_DELAY);

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3506_ctrl_senddata
 ****************************************************************************/

static int rk3506_ctrl_senddata(struct rk3506_usbhost_s *priv,
                                struct rk3506_ctrlinfo_s *ep0,
                                uint8_t *buffer, unsigned int buflen)
{
  struct rk3506_chan_s *chan = &priv->chan[ep0->outndx];
  int ret;

  chan->buffer = buffer;
  chan->buflen = buflen;
  chan->xfrd   = 0;

  if (buflen == 0)
    {
      chan->outdata1 = true;
    }

  chan->pid = chan->outdata1 ? DWC2_PID_DATA1 : DWC2_PID_DATA0;

  ret = rk3506_chan_waitsetup(priv, chan);
  if (ret < 0)
    {
      return ret;
    }

  rk3506_transfer_start(priv, ep0->outndx);

  return rk3506_chan_wait(priv, chan);
}

/****************************************************************************
 * Name: rk3506_ctrl_recvdata
 ****************************************************************************/

static int rk3506_ctrl_recvdata(struct rk3506_usbhost_s *priv,
                                struct rk3506_ctrlinfo_s *ep0,
                                uint8_t *buffer, unsigned int buflen)
{
  struct rk3506_chan_s *chan = &priv->chan[ep0->inndx];
  int ret;

  chan->pid    = DWC2_PID_DATA1;
  chan->buffer = buffer;
  chan->buflen = buflen;
  chan->xfrd   = 0;

  ret = rk3506_chan_waitsetup(priv, chan);
  if (ret < 0)
    {
      return ret;
    }

  rk3506_transfer_start(priv, ep0->inndx);

  return rk3506_chan_wait(priv, chan);
}

/****************************************************************************
 * Name: rk3506_in_setup
 ****************************************************************************/

static int rk3506_in_setup(struct rk3506_usbhost_s *priv, int chidx)
{
  struct rk3506_chan_s *chan;

  chan = &priv->chan[chidx];
  switch (chan->eptype)
    {
    default:
    case DWC2_EPTYPE_CTRL:
      {
        return -ENOSYS;
      }

    case DWC2_EPTYPE_ISOC:
      {
        chan->pid = DWC2_PID_DATA0;
      }
      break;

    case DWC2_EPTYPE_BULK:
      {
        chan->pid = chan->indata1 ? DWC2_PID_DATA1 : DWC2_PID_DATA0;
      }
      break;

    case DWC2_EPTYPE_INTR:
      {
        chan->pid = chan->indata1 ? DWC2_PID_DATA1 : DWC2_PID_DATA0;
      }
      break;
    }

  rk3506_transfer_start(priv, chidx);
  return OK;
}

/****************************************************************************
 * Name: rk3506_in_transfer
 ****************************************************************************/

static ssize_t rk3506_in_transfer(struct rk3506_usbhost_s *priv, int chidx,
                                  uint8_t *buffer, size_t buflen)
{
  struct rk3506_chan_s *chan;
  clock_t start;
  ssize_t xfrd;
  int ret;

  chan         = &priv->chan[chidx];
  chan->buffer = buffer;
  chan->buflen = buflen;
  chan->xfrd   = 0;
  xfrd         = 0;

  start = clock_systime_ticks();
  while (chan->xfrd < chan->buflen)
    {
      ret = rk3506_chan_waitsetup(priv, chan);
      if (ret < 0)
        {
          return (ssize_t)ret;
        }

      ret = rk3506_in_setup(priv, chidx);
      if (ret < 0)
        {
          uerr("ERROR: rk3506_in_setup failed: %d\n", ret);
          return (ssize_t)ret;
        }

      ret = rk3506_chan_wait(priv, chan);

      if (ret < 0)
        {
          if (ret == -EAGAIN)
            {
              if (xfrd > 0)
                {
                  return xfrd;
                }
              else
                {
                  useconds_t delay;
                  clock_t elapsed = clock_systime_ticks() - start;
                  if (elapsed >= RK3506_DATANAK_DELAY)
                    {
                      return (ssize_t)ret;
                    }

                  if (chan->eptype == DWC2_EPTYPE_INTR)
                    {
                      if (chan->interval > 0)
                        {
                          delay = (useconds_t)chan->interval * 1000;
                        }
                      else
                        {
                          delay = 1000;
                        }
                    }
                  else
                    {
                      delay = 1000;
                    }

                  if (delay > CONFIG_USEC_PER_TICK)
                    {
                      nxsig_usleep(delay - CONFIG_USEC_PER_TICK);
                    }
                }
            }
          else
            {
              uerr("ERROR: rk3506_chan_wait failed: %d\n", ret);
              return (ssize_t)ret;
            }
        }
      else
        {
          xfrd += chan->xfrd;
        }
    }

  return xfrd;
}

/****************************************************************************
 * Name: rk3506_out_setup
 ****************************************************************************/

static int rk3506_out_setup(struct rk3506_usbhost_s *priv, int chidx)
{
  struct rk3506_chan_s *chan;

  chan = &priv->chan[chidx];
  switch (chan->eptype)
    {
    default:
    case DWC2_EPTYPE_CTRL:
      {
        return -ENOSYS;
      }

    case DWC2_EPTYPE_ISOC:
      {
        chan->pid = DWC2_PID_DATA0;
      }
      break;

    case DWC2_EPTYPE_BULK:
      {
        chan->pid = chan->outdata1 ? DWC2_PID_DATA1 : DWC2_PID_DATA0;
      }
      break;

    case DWC2_EPTYPE_INTR:
      {
        chan->pid = chan->outdata1 ? DWC2_PID_DATA1 : DWC2_PID_DATA0;
        chan->outdata1 ^= true;
      }
      break;
    }

  rk3506_transfer_start(priv, chidx);
  return OK;
}

/****************************************************************************
 * Name: rk3506_out_transfer
 ****************************************************************************/

static ssize_t rk3506_out_transfer(struct rk3506_usbhost_s *priv,
                                   int chidx, uint8_t *buffer,
                                   size_t buflen)
{
  struct rk3506_chan_s *chan;
  clock_t start;
  clock_t elapsed;
  size_t xfrlen;
  ssize_t xfrd;
  int ret;
  bool zlp;

  chan  = &priv->chan[chidx];
  start = clock_systime_ticks();
  xfrd  = 0;
  zlp   = (buflen == 0);

  while (buflen > 0 || zlp)
    {
      xfrlen       = MIN(chan->maxpacket, buflen);
      chan->buffer = buffer;
      chan->buflen = xfrlen;
      chan->xfrd   = 0;

      ret = rk3506_chan_waitsetup(priv, chan);
      if (ret < 0)
        {
          return (ssize_t)ret;
        }

      ret = rk3506_out_setup(priv, chidx);
      if (ret < 0)
        {
          uerr("ERROR: rk3506_out_setup failed: %d\n", ret);
          return (ssize_t)ret;
        }

      ret = rk3506_chan_wait(priv, chan);

      if (ret < 0)
        {
          elapsed = clock_systime_ticks() - start;
          if (ret != -EAGAIN ||
              elapsed >= RK3506_DATANAK_DELAY ||
              chan->xfrd > 0)
            {
              uerr("ERROR: rk3506_chan_wait failed: %d\n", ret);
              return (ssize_t)ret;
            }

          rk3506_flush_txfifos(priv, DWC2_GRSTCTL_TXFNUM_HALL);
          nxsig_usleep(20 * 1000);
        }
      else
        {
          buffer += xfrlen;
          buflen -= xfrlen;
          xfrd   += chan->xfrd;
          zlp     = false;
        }
    }

  return xfrd;
}

/****************************************************************************
 * Name: rk3506_gint_wrpacket
 ****************************************************************************/

static void rk3506_gint_wrpacket(struct rk3506_usbhost_s *priv,
                                 uint8_t *buffer, int chidx, int buflen)
{
  uint32_t fifo;
  uint32_t word;
  int buflen32;

  buflen32 = (buflen + 3) >> 2;

  fifo = DWC2_DFIFO_OFFSET(chidx);

  for (; buflen32 > 0; buflen32--)
    {
      memcpy(&word, buffer, sizeof(uint32_t));
      rk3506_putreg(priv, fifo, word);
      buffer += sizeof(uint32_t);
    }

  priv->chan[chidx].inflight += buflen;
}

/****************************************************************************
 * Name: rk3506_gint_hcinisr
 ****************************************************************************/

static inline void rk3506_gint_hcinisr(struct rk3506_usbhost_s *priv,
                                       int chidx)
{
  struct rk3506_chan_s *chan = &priv->chan[chidx];
  uint32_t regval;
  uint32_t pending;

  pending = rk3506_getreg(priv, DWC2_HCINT_OFFSET(chidx));
  regval  = rk3506_getreg(priv, DWC2_HCINTMSK_OFFSET(chidx));
  pending &= regval;

  /* ACK */

  if ((pending & DWC2_HCINT_ACK) != 0)
    {
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_ACK);
      chan->progress++;

      /* Control/bulk IN: ACK asserts per received data packet.  If all
       * expected data is in (or a short packet terminated the transfer),
       * finish the transfer even though the core has not (yet) asserted
       * XFRC - observed on this core after NAK-retry sequences: the data
       * sits complete in the buffer while the channel idles until the
       * transfer is cancelled.  Clear the stale raw status, halt with
       * CHREASON_XFRC and let the CHH path report completion. */

      if ((chan->eptype == DWC2_EPTYPE_CTRL ||
           chan->eptype == DWC2_EPTYPE_BULK) &&
          (chan->xfrd == chan->buflen ||
           (chan->xfrd > 0 && (chan->xfrd % chan->maxpacket) != 0)))
        {
          rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), 0xffffffff);
          rk3506_chan_halt(priv, chidx, CHREASON_XFRC);

          /* The raw status bits are gone from the hardware: make sure
           * the rest of this handler does not act on the stale copy. */

          pending = 0;
        }
    }

  /* STALL */

  else if ((pending & DWC2_HCINT_STALL) != 0)
    {
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx),
                    DWC2_HCINT_NAK | DWC2_HCINT_STALL);
      rk3506_chan_halt(priv, chidx, CHREASON_STALL);
      pending &= ~DWC2_HCINT_NAK;
    }

  /* DTERR */

  else if ((pending & DWC2_HCINT_DTERR) != 0)
    {
      rk3506_chan_halt(priv, chidx, CHREASON_DTERR);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx),
                    DWC2_HCINT_NAK | DWC2_HCINT_DTERR);
    }

  /* FRMOR */

  if ((pending & DWC2_HCINT_FRMOR) != 0)
    {
      rk3506_chan_halt(priv, chidx, CHREASON_FRMOR);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_FRMOR);
    }

  /* XFRC */

  else if ((pending & DWC2_HCINT_XFRC) != 0)
    {
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_XFRC);

      if (chan->eptype == DWC2_EPTYPE_CTRL ||
          chan->eptype == DWC2_EPTYPE_BULK)
        {
          rk3506_chan_halt(priv, chidx, CHREASON_XFRC);
          rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_NAK);
        }
      else if (chan->eptype == DWC2_EPTYPE_INTR)
        {
          regval = rk3506_getreg(priv, DWC2_HCCHAR_OFFSET(chidx));
          regval |= DWC2_HCCHAR_ODDFRM;
          rk3506_putreg(priv, DWC2_HCCHAR_OFFSET(chidx), regval);
          chan->result = OK;
        }
    }

  /* CHH */

  else if ((pending & DWC2_HCINT_CHH) != 0)
    {
      regval  = rk3506_getreg(priv, DWC2_HCINTMSK_OFFSET(chidx));
      regval &= ~DWC2_HCINT_CHH;
      rk3506_putreg(priv, DWC2_HCINTMSK_OFFSET(chidx), regval);

      if (chan->chreason == CHREASON_XFRC)
        {
          chan->result = OK;
        }
      else if (chan->chreason == CHREASON_STALL)
        {
          chan->result = EPERM;
        }
      else if ((chan->chreason == CHREASON_TXERR) ||
               (chan->chreason == CHREASON_DTERR))
        {
          chan->result = EIO;
        }
      else if (chan->chreason == CHREASON_NAK ||
               chan->chreason == CHREASON_NYET)
        {
          chan->result = EAGAIN;
        }
      else
        {
          chan->result = EPIPE;
        }

      uinfo("ch%d IN done: reason=%u result=%d xfrd=%u\n",
            chidx, (unsigned)chan->chreason, chan->result,
            (unsigned)chan->xfrd);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_CHH);
    }

  /* TXERR */

  else if ((pending & DWC2_HCINT_TXERR) != 0)
    {
      rk3506_chan_halt(priv, chidx, CHREASON_TXERR);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_TXERR);
    }

  /* NYET (high-speed flow control; core halts the channel) */

  else if ((pending & DWC2_HCINT_NYET) != 0)
    {
      chan->progress++;

      if (chan->eptype == DWC2_EPTYPE_CTRL ||
          chan->eptype == DWC2_EPTYPE_BULK)
        {
          rk3506_chan_halt(priv, chidx, CHREASON_NYET);
        }

      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_NYET);
    }

  /* NAK */

  else if ((pending & DWC2_HCINT_NAK) != 0)
    {
      chan->progress++;

      if (chan->eptype == DWC2_EPTYPE_INTR ||
          chan->eptype == DWC2_EPTYPE_BULK)
        {
          rk3506_chan_halt(priv, chidx, CHREASON_NAK);
        }
      else if (chan->eptype == DWC2_EPTYPE_CTRL)
        {
          regval  = rk3506_getreg(priv, DWC2_HCCHAR_OFFSET(chidx));
          regval |= DWC2_HCCHAR_CHENA;
          regval &= ~DWC2_HCCHAR_CHDIS;
          rk3506_putreg(priv, DWC2_HCCHAR_OFFSET(chidx), regval);
        }

      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_NAK);
    }

  rk3506_chan_wakeup(priv, chan);
}

/****************************************************************************
 * Name: rk3506_gint_hcoutisr
 ****************************************************************************/

static inline void rk3506_gint_hcoutisr(struct rk3506_usbhost_s *priv,
                                        int chidx)
{
  struct rk3506_chan_s *chan = &priv->chan[chidx];
  uint32_t regval;
  uint32_t pending;

  pending = rk3506_getreg(priv, DWC2_HCINT_OFFSET(chidx));
  regval  = rk3506_getreg(priv, DWC2_HCINTMSK_OFFSET(chidx));
  pending &= regval;

  if ((pending & DWC2_HCINT_ACK) != 0)
    {
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_ACK);
      chan->progress++;
    }
  else if ((pending & DWC2_HCINT_FRMOR) != 0)
    {
      rk3506_chan_halt(priv, chidx, CHREASON_FRMOR);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_FRMOR);
    }
  else if ((pending & DWC2_HCINT_XFRC) != 0)
    {
      priv->chan[chidx].buffer  += priv->chan[chidx].inflight;
      priv->chan[chidx].xfrd    += priv->chan[chidx].inflight;
      priv->chan[chidx].inflight = 0;

      rk3506_chan_halt(priv, chidx, CHREASON_XFRC);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_XFRC);
    }
  else if ((pending & DWC2_HCINT_STALL) != 0)
    {
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_STALL);
      rk3506_chan_halt(priv, chidx, CHREASON_STALL);
    }
  else if ((pending & DWC2_HCINT_NYET) != 0)
    {
      /* High-speed flow control: the endpoint accepted this packet but
       * is not ready for the next one.  The core self-halts the
       * channel; re-issue the remaining transfer (EAGAIN path). */

      chan->progress++;
      rk3506_chan_halt(priv, chidx, CHREASON_NYET);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_NYET);
    }
  else if ((pending & DWC2_HCINT_NAK) != 0)
    {
      chan->progress++;
      rk3506_chan_halt(priv, chidx, CHREASON_NAK);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_NAK);
    }
  else if ((pending & DWC2_HCINT_TXERR) != 0)
    {
      rk3506_chan_halt(priv, chidx, CHREASON_TXERR);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_TXERR);
    }
  else if (pending & DWC2_HCINT_DTERR)
    {
      rk3506_chan_halt(priv, chidx, CHREASON_DTERR);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx),
                    DWC2_HCINT_DTERR | DWC2_HCINT_NAK);
    }
  else if ((pending & DWC2_HCINT_CHH) != 0)
    {
      regval  = rk3506_getreg(priv, DWC2_HCINTMSK_OFFSET(chidx));
      regval &= ~DWC2_HCINT_CHH;
      rk3506_putreg(priv, DWC2_HCINTMSK_OFFSET(chidx), regval);

      if (chan->chreason == CHREASON_XFRC)
        {
          chan->result = OK;

          regval = rk3506_getreg(priv, DWC2_HCCHAR_OFFSET(chidx));
          if ((regval & DWC2_HCCHAR_EPTYP_MASK) ==
               DWC2_HCCHAR_EPTYP_BULK &&
              (chan->npackets & 1) != 0)
            {
              chan->outdata1 ^= true;
            }
        }
      else if (chan->chreason == CHREASON_NAK ||
               chan->chreason == CHREASON_NYET)
        {
          chan->result = EAGAIN;
        }
      else if (chan->chreason == CHREASON_STALL)
        {
          chan->result = EPERM;
        }
      else if ((chan->chreason == CHREASON_TXERR) ||
               (chan->chreason == CHREASON_DTERR))
        {
          chan->result = EIO;
        }
      else
        {
          chan->result = EPIPE;
        }

      uinfo("ch%d OUT done: reason=%u result=%d xfrd=%u\n",
            chidx, (unsigned)chan->chreason, chan->result,
            (unsigned)chan->xfrd);
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(chidx), DWC2_HCINT_CHH);
    }

  rk3506_chan_wakeup(priv, chan);
}

/****************************************************************************
 * Name: rk3506_gint_connected
 ****************************************************************************/

static void rk3506_gint_connected(struct rk3506_usbhost_s *priv)
{
  if (!priv->connected)
    {
      priv->connected = true;
      priv->change    = true;
      DEBUGASSERT(priv->smstate == SMSTATE_DETACHED);

      priv->smstate = SMSTATE_ATTACHED;
      if (priv->pscwait)
        {
          nxsem_post(&priv->pscsem);
          priv->pscwait = false;
        }
    }
}

/****************************************************************************
 * Name: rk3506_gint_disconnected
 ****************************************************************************/

static void rk3506_gint_disconnected(struct rk3506_usbhost_s *priv)
{
  if (priv->connected)
    {
      if (priv->rhport.hport.devclass)
        {
          CLASS_DISCONNECTED(priv->rhport.hport.devclass);
          priv->rhport.hport.devclass = NULL;
        }

      priv->smstate   = SMSTATE_DETACHED;
      priv->connected = false;
      priv->change    = true;
      rk3506_chan_freeall(priv);

      priv->rhport.hport.speed = USB_SPEED_FULL;
      priv->rhport.hport.funcaddr = 0;

      if (priv->pscwait)
        {
          nxsem_post(&priv->pscsem);
          priv->pscwait = false;
        }
    }
}

/****************************************************************************
 * Name: rk3506_gint_rxflvlisr
 ****************************************************************************/

static inline void rk3506_gint_rxflvlisr(struct rk3506_usbhost_s *priv)
{
  uint8_t  *dest;
  uint32_t grxsts;
  uint32_t intmsk;
  uint32_t hcchar;
  uint32_t hctsiz;
  uint32_t fifo;
  int bcnt;
  int bcnt32;
  int chidx;
  int i;

  intmsk  = rk3506_getreg(priv, DWC2_GINTMSK_OFFSET);
  intmsk &= ~DWC2_GINT_RXFLVL;
  rk3506_putreg(priv, DWC2_GINTMSK_OFFSET, intmsk);

  grxsts = rk3506_getreg(priv, DWC2_GRXSTSP_OFFSET);

  chidx = (grxsts & DWC2_GRXSTSH_CHNUM_MASK) >>
          DWC2_GRXSTSH_CHNUM_SHIFT;

  hcchar = rk3506_getreg(priv, DWC2_HCCHAR_OFFSET(chidx));

  switch (grxsts & DWC2_GRXSTSH_PKTSTS_MASK)
    {
    case DWC2_GRXSTSH_PKTSTS_INRECVD:
      {
        bcnt = (grxsts & DWC2_GRXSTSH_BCNT_MASK) >>
               DWC2_GRXSTSH_BCNT_SHIFT;
        uinfo("rxflvl: ch%d INRECVD bcnt=%d\n", chidx, bcnt);
        priv->chan[chidx].progress++;
        if (bcnt > 0 && priv->chan[chidx].buffer != NULL)
          {
            dest   = priv->chan[chidx].buffer;
            fifo   = DWC2_DFIFO_OFFSET(0);
            bcnt32 = (bcnt + 3) >> 2;

            for (i = 0; i < bcnt32; i++)
              {
                uint32_t word = rk3506_getreg(priv, fifo);
                memcpy(dest, &word, sizeof(uint32_t));
                dest += sizeof(uint32_t);
              }

            priv->chan[chidx].indata1 ^= true;

            priv->chan[chidx].buffer += bcnt;
            priv->chan[chidx].xfrd   += bcnt;

            hctsiz = rk3506_getreg(priv, DWC2_HCTSIZ_OFFSET(chidx));
            if ((hctsiz & DWC2_HCTSIZ_PKTCNT_MASK) != 0)
              {
                hcchar |= DWC2_HCCHAR_CHENA;
                hcchar &= ~DWC2_HCCHAR_CHDIS;
                rk3506_putreg(priv, DWC2_HCCHAR_OFFSET(chidx), hcchar);
              }
          }
      }
      break;

    case DWC2_GRXSTSH_PKTSTS_INDONE:
    case DWC2_GRXSTSH_PKTSTS_DTOGERR:
    case DWC2_GRXSTSH_PKTSTS_HALTED:
    default:
      break;
    }

  intmsk |= DWC2_GINT_RXFLVL;
  rk3506_putreg(priv, DWC2_GINTMSK_OFFSET, intmsk);
}

/****************************************************************************
 * Name: rk3506_gint_nptxfeisr
 ****************************************************************************/

static inline void rk3506_gint_nptxfeisr(struct rk3506_usbhost_s *priv)
{
  struct rk3506_chan_s *chan;
  uint32_t     regval;
  unsigned int wrsize;
  unsigned int avail;
  unsigned int chidx;

  chidx = priv->chidx;
  chan  = &priv->chan[chidx];

  chan->buffer  += chan->inflight;
  chan->xfrd    += chan->inflight;
  chan->inflight = 0;

  if (chan->xfrd >= chan->buflen)
    {
      rk3506_modifyreg(priv, DWC2_GINTMSK_OFFSET, DWC2_GINT_NPTXFE, 0);
      return;
    }

  regval = rk3506_getreg(priv, DWC2_HNPTXSTS_OFFSET);
  avail = ((regval & DWC2_HNPTXSTS_NPTXFSAV_MASK) >>
           DWC2_HNPTXSTS_NPTXFSAV_SHIFT) << 2;

  wrsize = chan->buflen - chan->xfrd;

  DEBUGASSERT(wrsize > 0 && avail >= MIN(wrsize, chan->maxpacket));
  if (wrsize > avail)
    {
      unsigned int wrpackets = avail / chan->maxpacket;
      wrsize = wrpackets * chan->maxpacket;
    }
  else
    {
      rk3506_modifyreg(priv, DWC2_GINTMSK_OFFSET, DWC2_GINT_NPTXFE, 0);
    }

  rk3506_gint_wrpacket(priv, chan->buffer, chidx, wrsize);
}

/****************************************************************************
 * Name: rk3506_gint_ptxfeisr
 ****************************************************************************/

static inline void rk3506_gint_ptxfeisr(struct rk3506_usbhost_s *priv)
{
  struct rk3506_chan_s *chan;
  uint32_t     regval;
  unsigned int wrsize;
  unsigned int avail;
  unsigned int chidx;

  chidx = priv->chidx;
  chan  = &priv->chan[chidx];

  chan->buffer  += chan->inflight;
  chan->xfrd    += chan->inflight;
  chan->inflight = 0;

  if (chan->xfrd >= chan->buflen)
    {
      rk3506_modifyreg(priv, DWC2_GINTMSK_OFFSET, DWC2_GINT_PTXFE, 0);
      return;
    }

  regval = rk3506_getreg(priv, DWC2_HPTXSTS_OFFSET);
  avail = ((regval & DWC2_HPTXSTS_PTXFSAVL_MASK) >>
           DWC2_HPTXSTS_PTXFSAVL_SHIFT) << 2;

  wrsize = chan->buflen - chan->xfrd;

  DEBUGASSERT(wrsize && avail >= MIN(wrsize, chan->maxpacket));
  if (wrsize > avail)
    {
      unsigned int wrpackets = avail / chan->maxpacket;
      wrsize = wrpackets * chan->maxpacket;
    }
  else
    {
      rk3506_modifyreg(priv, DWC2_GINTMSK_OFFSET, DWC2_GINT_PTXFE, 0);
    }

  rk3506_gint_wrpacket(priv, chan->buffer, chidx, wrsize);
}

/****************************************************************************
 * Name: rk3506_gint_hcisr
 ****************************************************************************/

static inline void rk3506_gint_hcisr(struct rk3506_usbhost_s *priv)
{
  uint32_t haint;
  uint32_t hcchar;
  int i;

  haint = rk3506_getreg(priv, DWC2_HAINT_OFFSET);
  for (i = 0; i < RK3506_DWC2_NHOST_CHANNELS; i++)
    {
      if ((haint & DWC2_HAINT(i)) != 0)
        {
          hcchar = rk3506_getreg(priv, DWC2_HCCHAR_OFFSET(i));

          if ((hcchar & DWC2_HCCHAR_EPDIR) != 0)
            {
              rk3506_gint_hcinisr(priv, i);
            }
          else
            {
              rk3506_gint_hcoutisr(priv, i);
            }
        }
    }
}

/****************************************************************************
 * Name: rk3506_gint_hprtisr
 ****************************************************************************/

static inline void rk3506_gint_hprtisr(struct rk3506_usbhost_s *priv)
{
  uint32_t hprt;
  uint32_t newhprt;
  uint32_t hcfg;

  hprt = rk3506_getreg(priv, DWC2_HPRT_OFFSET);

  newhprt = hprt & ~(DWC2_HPRT_PENA    | DWC2_HPRT_PCDET  |
                     DWC2_HPRT_PENCHNG | DWC2_HPRT_POCCHNG);

  if ((hprt & DWC2_HPRT_POCCHNG) != 0)
    {
      newhprt |= DWC2_HPRT_POCCHNG;
    }

  if ((hprt & DWC2_HPRT_PCDET) != 0)
    {
      newhprt |= DWC2_HPRT_PCDET;
      rk3506_portreset(priv);
      rk3506_gint_connected(priv);
    }

  if ((hprt & DWC2_HPRT_PENCHNG) != 0)
    {
      newhprt |= DWC2_HPRT_PENCHNG;

      if ((hprt & DWC2_HPRT_PENA) != 0)
        {
          rk3506_gint_connected(priv);

          hcfg = rk3506_getreg(priv, DWC2_HCFG_OFFSET);

          if ((hprt & DWC2_HPRT_PSPD_MASK) == DWC2_HPRT_PSPD_LS)
            {
              rk3506_putreg(priv, DWC2_HFIR_OFFSET, 6000);

              if ((hcfg & DWC2_HCFG_FSLSPCS_MASK) !=
                  (2 << DWC2_HCFG_FSLSPCS_SHIFT))
                {
                  hcfg &= ~DWC2_HCFG_FSLSPCS_MASK;
                  hcfg |= (2 << DWC2_HCFG_FSLSPCS_SHIFT);
                  rk3506_putreg(priv, DWC2_HCFG_OFFSET, hcfg);
                  rk3506_portreset(priv);
                }
            }
          else
            {
              rk3506_putreg(priv, DWC2_HFIR_OFFSET, 48000);

              if ((hcfg & DWC2_HCFG_FSLSPCS_MASK) !=
                  DWC2_HCFG_FSLSPCS_FS48MHz)
                {
                  hcfg &= ~DWC2_HCFG_FSLSPCS_MASK;
                  hcfg |= DWC2_HCFG_FSLSPCS_FS48MHz;
                  rk3506_putreg(priv, DWC2_HCFG_OFFSET, hcfg);
                  rk3506_portreset(priv);
                }
            }
        }
    }

  rk3506_putreg(priv, DWC2_HPRT_OFFSET, newhprt);
}

/****************************************************************************
 * Name: rk3506_gint_discisr
 ****************************************************************************/

static inline void rk3506_gint_discisr(struct rk3506_usbhost_s *priv)
{
  rk3506_gint_disconnected(priv);
  rk3506_putreg(priv, DWC2_GINTSTS_OFFSET, DWC2_GINT_DISC);
}

/****************************************************************************
 * Name: rk3506_gint_ipxfrisr
 ****************************************************************************/

static inline void rk3506_gint_ipxfrisr(struct rk3506_usbhost_s *priv)
{
  uint32_t regval;

  regval = rk3506_getreg(priv, DWC2_HCCHAR_OFFSET(0));
  regval |= (DWC2_HCCHAR_CHDIS | DWC2_HCCHAR_CHENA);
  rk3506_putreg(priv, DWC2_HCCHAR_OFFSET(0), regval);

  rk3506_putreg(priv, DWC2_GINTSTS_OFFSET, DWC2_GINT_IPXFR);
}

/****************************************************************************
 * Name: rk3506_gint_isr
 ****************************************************************************/

static int rk3506_gint_isr(int irq, void *context, void *arg)
{
  struct rk3506_usbhost_s *priv = (struct rk3506_usbhost_s *)arg;
  uint32_t pending;

  for (; ; )
    {
      pending  = rk3506_getreg(priv, DWC2_GINTSTS_OFFSET);
      pending &= rk3506_getreg(priv, DWC2_GINTMSK_OFFSET);

      if (pending == 0)
        {
          return OK;
        }

      if ((pending & DWC2_GINT_RXFLVL) != 0)
        {
          rk3506_gint_rxflvlisr(priv);
        }

      if ((pending & DWC2_GINT_NPTXFE) != 0)
        {
          rk3506_gint_nptxfeisr(priv);
        }

      if ((pending & DWC2_GINT_PTXFE) != 0)
        {
          rk3506_gint_ptxfeisr(priv);
        }

      if ((pending & DWC2_GINT_HC) != 0)
        {
          rk3506_gint_hcisr(priv);
        }

      if ((pending & DWC2_GINT_HPRT) != 0)
        {
          rk3506_gint_hprtisr(priv);
        }

      if ((pending & DWC2_GINT_DISC) != 0)
        {
          rk3506_gint_discisr(priv);
        }

      if ((pending & DWC2_GINT_IPXFR) != 0)
        {
          rk3506_gint_ipxfrisr(priv);
        }
    }
}

/****************************************************************************
 * Name: rk3506_gint_enable / rk3506_gint_disable
 ****************************************************************************/

static void rk3506_gint_enable(struct rk3506_usbhost_s *priv)
{
  rk3506_modifyreg(priv, DWC2_GAHBCFG_OFFSET, 0, DWC2_GAHBCFG_GINTMSK);
}

static void rk3506_gint_disable(struct rk3506_usbhost_s *priv)
{
  rk3506_modifyreg(priv, DWC2_GAHBCFG_OFFSET, DWC2_GAHBCFG_GINTMSK, 0);
}

/****************************************************************************
 * Name: rk3506_hostinit_enable
 ****************************************************************************/

static inline void rk3506_hostinit_enable(struct rk3506_usbhost_s *priv)
{
  uint32_t regval;

  rk3506_putreg(priv, DWC2_GINTMSK_OFFSET, 0);
  rk3506_putreg(priv, DWC2_GINTSTS_OFFSET, 0xffffffff);
  rk3506_putreg(priv, DWC2_GOTGINT_OFFSET, 0xffffffff);
  rk3506_putreg(priv, DWC2_GINTSTS_OFFSET, 0xbfffffff);

  regval = (DWC2_GINT_WKUP | DWC2_GINT_USBSUSP |
            DWC2_GINT_RXFLVL | DWC2_GINT_IPXFR  |
            DWC2_GINT_HPRT   | DWC2_GINT_HC     |
            DWC2_GINT_DISC);
  rk3506_putreg(priv, DWC2_GINTMSK_OFFSET, regval);
}

/****************************************************************************
 * Name: rk3506_txfe_enable
 ****************************************************************************/

static void rk3506_txfe_enable(struct rk3506_usbhost_s *priv, int chidx)
{
  struct rk3506_chan_s *chan = &priv->chan[chidx];
  irqstate_t flags;
  uint32_t regval;

  flags = enter_critical_section();

  regval = rk3506_getreg(priv, DWC2_GINTMSK_OFFSET);
  switch (chan->eptype)
    {
    default:
    case DWC2_EPTYPE_CTRL:
    case DWC2_EPTYPE_BULK:
      regval |= DWC2_GINT_NPTXFE;
      break;

    case DWC2_EPTYPE_INTR:
    case DWC2_EPTYPE_ISOC:
      regval |= DWC2_GINT_PTXFE;
      break;
    }

  rk3506_putreg(priv, DWC2_GINTMSK_OFFSET, regval);
  leave_critical_section(flags);
}

/****************************************************************************
 * USB Host Controller Operations
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_wait
 ****************************************************************************/

static int rk3506_wait(struct usbhost_connection_s *conn,
                       struct usbhost_hubport_s **hport)
{
  struct rk3506_usbhost_s *priv = container_of(conn,
                                               struct rk3506_usbhost_s,
                                               conn);
  struct usbhost_hubport_s *connport;
  irqstate_t flags;
  int ret;

  flags = enter_critical_section();
  for (; ; )
    {
      if (priv->change)
        {
          connport = &priv->rhport.hport;
          connport->connected = priv->connected;
          priv->change = false;

          *hport = connport;
          leave_critical_section(flags);

          uinfo("RHport Connected: %s\n",
                connport->connected ? "YES" : "NO");
          return OK;
        }

#ifdef CONFIG_USBHOST_HUB
      if (priv->hport)
        {
          connport = (struct usbhost_hubport_s *)priv->hport;
          priv->hport = NULL;

          *hport = connport;
          leave_critical_section(flags);

          uinfo("Hub port Connected: %s\n",
                connport->connected ? "YES" : "NO");
          return OK;
        }
#endif

      priv->pscwait = true;
      ret = nxsem_wait_uninterruptible(&priv->pscsem);
      if (ret < 0)
        {
          return ret;
        }
    }
}

/****************************************************************************
 * Name: rk3506_rh_enumerate
 ****************************************************************************/

static int rk3506_rh_enumerate(struct rk3506_usbhost_s *priv,
                               struct usbhost_connection_s *conn,
                               struct usbhost_hubport_s *hport)
{
  uint32_t regval;
  int ret;

  DEBUGASSERT(conn != NULL && hport != NULL && hport->port == 0);

  if (!priv->connected)
    {
      return -ENODEV;
    }

  DEBUGASSERT(priv->smstate == SMSTATE_ATTACHED);

  nxsig_usleep(100 * 1000);

  rk3506_portreset(priv);

  regval = rk3506_getreg(priv, DWC2_HPRT_OFFSET);
  if ((regval & DWC2_HPRT_PSPD_MASK) == DWC2_HPRT_PSPD_LS)
    {
      priv->rhport.hport.speed = USB_SPEED_LOW;
    }
  else
    {
      priv->rhport.hport.speed = USB_SPEED_FULL;
    }

  ret = rk3506_ctrlchan_alloc(priv, 0, 0, priv->rhport.hport.speed,
                              &priv->ep0);
  if (ret < 0)
    {
      uerr("ERROR: Failed to allocate a control endpoint: %d\n", ret);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3506_enumerate
 ****************************************************************************/

static int rk3506_enumerate(struct usbhost_connection_s *conn,
                            struct usbhost_hubport_s *hport)
{
  struct rk3506_usbhost_s *priv = container_of(conn,
                                               struct rk3506_usbhost_s,
                                               conn);
  int ret;

  DEBUGASSERT(hport);

#ifdef CONFIG_USBHOST_HUB
  if (ROOTHUB(hport))
#endif
    {
      ret = rk3506_rh_enumerate(priv, conn, hport);
      if (ret < 0)
        {
          return ret;
        }
    }

  uinfo("Enumerate the device\n");
  priv->smstate = SMSTATE_ENUM;
  ret = usbhost_enumerate(hport, &hport->devclass);

  if (ret < 0)
    {
      uerr("ERROR: Enumeration failed: %d\n", ret);
      rk3506_gint_disconnected(priv);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3506_ep0configure
 ****************************************************************************/

static int rk3506_ep0configure(struct usbhost_driver_s *drvr,
                               usbhost_ep_t ep0, uint8_t funcaddr,
                               uint8_t speed, uint16_t maxpacketsize)
{
  struct rk3506_usbhost_s *priv = (struct rk3506_usbhost_s *)drvr;
  struct rk3506_ctrlinfo_s *ep0info = (struct rk3506_ctrlinfo_s *)ep0;
  struct rk3506_chan_s *chan;
  int ret;

  DEBUGASSERT(drvr != NULL && ep0info != NULL);

  if (drvr == NULL || ep0info == NULL)
    {
      return -EINVAL;
    }

  /* The usbhost core takes maxpacketsize straight from the device
   * descriptor's bMaxPacketSize0 field.  A phantom / broken device can
   * report garbage here; panicking the kernel over it is worse than
   * clamping (Linux hub.c applies the same EP0 clamp policy).  With a
   * clamped EP0 the following control transfer simply fails and
   * enumeration bails out gracefully.
   */

  if (maxpacketsize > RK3506_EP0_MAX_PACKET_SIZE)
    {
      uwarn("WARNING: EP0 maxpacketsize %u > %u, clamping\n",
            maxpacketsize, RK3506_EP0_MAX_PACKET_SIZE);
      maxpacketsize = RK3506_EP0_MAX_PACKET_SIZE;
    }

  if (funcaddr >= 128)
    {
      uerr("ERROR: EP0 configure: invalid funcaddr %u\n", funcaddr);
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  chan            = &priv->chan[ep0info->outndx];
  chan->funcaddr  = funcaddr;
  chan->speed     = speed;
  chan->maxpacket = maxpacketsize;
  rk3506_chan_configure(priv, ep0info->outndx);

  chan            = &priv->chan[ep0info->inndx];
  chan->funcaddr  = funcaddr;
  chan->speed     = speed;
  chan->maxpacket = maxpacketsize;
  rk3506_chan_configure(priv, ep0info->inndx);

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3506_epalloc
 ****************************************************************************/

static int rk3506_epalloc(struct usbhost_driver_s *drvr,
                          const struct usbhost_epdesc_s *epdesc,
                          usbhost_ep_t *ep)
{
  struct rk3506_usbhost_s *priv = (struct rk3506_usbhost_s *)drvr;
  int ret;

  DEBUGASSERT(drvr != 0 && epdesc != NULL && ep != NULL);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (epdesc->xfrtype == DWC2_EPTYPE_CTRL)
    {
      ret = rk3506_ctrlep_alloc(priv, epdesc, ep);
    }
  else
    {
      ret = rk3506_xfrep_alloc(priv, epdesc, ep);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3506_epfree
 ****************************************************************************/

static int rk3506_epfree(struct usbhost_driver_s *drvr, usbhost_ep_t ep)
{
  struct rk3506_usbhost_s *priv = (struct rk3506_usbhost_s *)drvr;
  int ret;

  DEBUGASSERT(priv);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if ((uintptr_t)ep < RK3506_MAX_TX_FIFOS)
    {
      rk3506_chan_free(priv, (int)ep);
    }
  else
    {
      struct rk3506_ctrlinfo_s *ctrlep =
        (struct rk3506_ctrlinfo_s *)ep;

      rk3506_chan_free(priv, ctrlep->inndx);
      rk3506_chan_free(priv, ctrlep->outndx);
      kmm_free(ctrlep);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3506_alloc
 ****************************************************************************/

static int rk3506_alloc(struct usbhost_driver_s *drvr,
                        uint8_t **buffer, size_t *maxlen)
{
  uint8_t *alloc;

  DEBUGASSERT(drvr && buffer && maxlen);

  alloc = kmm_malloc(CONFIG_RK3506_DWC2_DESCSIZE);
  if (!alloc)
    {
      return -ENOMEM;
    }

  *buffer = alloc;
  *maxlen = CONFIG_RK3506_DWC2_DESCSIZE;
  return OK;
}

/****************************************************************************
 * Name: rk3506_free
 ****************************************************************************/

static int rk3506_free(struct usbhost_driver_s *drvr, uint8_t *buffer)
{
  DEBUGASSERT(drvr && buffer);
  kmm_free(buffer);
  return OK;
}

/****************************************************************************
 * Name: rk3506_ioalloc
 ****************************************************************************/

static int rk3506_ioalloc(struct usbhost_driver_s *drvr,
                          uint8_t **buffer, size_t buflen)
{
  uint8_t *alloc;

  DEBUGASSERT(drvr && buffer && buflen > 0);

  alloc = kmm_malloc(buflen);
  if (!alloc)
    {
      return -ENOMEM;
    }

  *buffer = alloc;
  return OK;
}

/****************************************************************************
 * Name: rk3506_iofree
 ****************************************************************************/

static int rk3506_iofree(struct usbhost_driver_s *drvr, uint8_t *buffer)
{
  DEBUGASSERT(drvr && buffer);
  kmm_free(buffer);
  return OK;
}

/****************************************************************************
 * Name: rk3506_ctrlin
 ****************************************************************************/

static int rk3506_ctrlin(struct usbhost_driver_s *drvr, usbhost_ep_t ep0,
                         const struct usb_ctrlreq_s *req,
                         uint8_t *buffer)
{
  struct rk3506_usbhost_s *priv = (struct rk3506_usbhost_s *)drvr;
  struct rk3506_ctrlinfo_s *ep0info = (struct rk3506_ctrlinfo_s *)ep0;
  uint16_t buflen;
  clock_t start;
  clock_t elapsed;
  int retries;
  int ret;

  DEBUGASSERT(priv != NULL && ep0info != NULL && req != NULL);

  buflen = rk3506_getle16(req->len);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  for (retries = 0; retries < RK3506_RETRY_COUNT; retries++)
    {
      ret = rk3506_ctrl_sendsetup(priv, ep0info, req);
      if (ret < 0)
        {
          continue;
        }

      if (buflen > 0)
        {
          ret = rk3506_ctrl_recvdata(priv, ep0info, buffer, buflen);
          if (ret < 0)
            {
              continue;
            }
        }

      start = clock_systime_ticks();
      do
        {
          priv->chan[ep0info->outndx].outdata1 ^= true;
          ret = rk3506_ctrl_senddata(priv, ep0info, NULL, 0);
          if (ret == OK)
            {
              nxmutex_unlock(&priv->lock);
              return OK;
            }

          elapsed = clock_systime_ticks() - start;
        }
      while (elapsed < RK3506_DATANAK_DELAY);
    }

  nxmutex_unlock(&priv->lock);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3506_ctrlout
 ****************************************************************************/

static int rk3506_ctrlout(struct usbhost_driver_s *drvr, usbhost_ep_t ep0,
                          const struct usb_ctrlreq_s *req,
                          const uint8_t *buffer)
{
  struct rk3506_usbhost_s *priv = (struct rk3506_usbhost_s *)drvr;
  struct rk3506_ctrlinfo_s *ep0info = (struct rk3506_ctrlinfo_s *)ep0;
  uint16_t buflen;
  clock_t start;
  clock_t elapsed;
  int retries;
  int ret;

  DEBUGASSERT(priv != NULL && ep0info != NULL && req != NULL);

  buflen = rk3506_getle16(req->len);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  for (retries = 0; retries < RK3506_RETRY_COUNT; retries++)
    {
      ret = rk3506_ctrl_sendsetup(priv, ep0info, req);
      if (ret < 0)
        {
          continue;
        }

      start = clock_systime_ticks();
      do
        {
          if (buflen > 0)
            {
              priv->chan[ep0info->outndx].outdata1 = true;
              ret = rk3506_ctrl_senddata(priv, ep0info,
                                        (uint8_t *)buffer, buflen);
            }

          if (ret == OK)
            {
              ret = rk3506_ctrl_recvdata(priv, ep0info, NULL, 0);
              if (ret == OK)
                {
                  nxmutex_unlock(&priv->lock);
                  return OK;
                }
            }

          elapsed = clock_systime_ticks() - start;
        }
      while (elapsed < RK3506_DATANAK_DELAY);
    }

  nxmutex_unlock(&priv->lock);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3506_transfer
 ****************************************************************************/

static ssize_t rk3506_transfer(struct usbhost_driver_s *drvr,
                               usbhost_ep_t ep, uint8_t *buffer,
                               size_t buflen)
{
  struct rk3506_usbhost_s *priv = (struct rk3506_usbhost_s *)drvr;
  unsigned int chidx = (unsigned int)ep;
  ssize_t nbytes;
  int ret;

  DEBUGASSERT(priv && buffer && chidx < RK3506_MAX_TX_FIFOS && buflen > 0);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return (ssize_t)ret;
    }

  if (priv->chan[chidx].in)
    {
      nbytes = rk3506_in_transfer(priv, chidx, buffer, buflen);
    }
  else
    {
      nbytes = rk3506_out_transfer(priv, chidx, buffer, buflen);
    }

  nxmutex_unlock(&priv->lock);
  return nbytes;
}

/****************************************************************************
 * Name: rk3506_cancel
 ****************************************************************************/

static int rk3506_cancel(struct usbhost_driver_s *drvr, usbhost_ep_t ep)
{
  struct rk3506_usbhost_s *priv = (struct rk3506_usbhost_s *)drvr;
  struct rk3506_chan_s *chan;
  unsigned int chidx = (unsigned int)ep;
  irqstate_t flags;

  DEBUGASSERT(priv && chidx < RK3506_MAX_TX_FIFOS);
  chan = &priv->chan[chidx];

  flags = enter_critical_section();

  rk3506_chan_halt(priv, chidx, CHREASON_CANCELLED);
  chan->result = -ESHUTDOWN;

  if (chan->waiter)
    {
      nxsem_post(&chan->waitsem);
      chan->waiter = false;
    }

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: rk3506_connect
 ****************************************************************************/

#ifdef CONFIG_USBHOST_HUB
static int rk3506_connect(struct usbhost_driver_s *drvr,
                          struct usbhost_hubport_s *hport,
                          bool connected)
{
  struct rk3506_usbhost_s *priv = (struct rk3506_usbhost_s *)drvr;
  irqstate_t flags;

  DEBUGASSERT(priv != NULL && hport != NULL);

  hport->connected = connected;

  flags = enter_critical_section();
  priv->hport = hport;
  if (priv->pscwait)
    {
      priv->pscwait = false;
      nxsem_post(&priv->pscsem);
    }

  leave_critical_section(flags);
  return OK;
}
#endif

/****************************************************************************
 * Name: rk3506_disconnect
 ****************************************************************************/

static void rk3506_disconnect(struct usbhost_driver_s *drvr,
                              struct usbhost_hubport_s *hport)
{
  DEBUGASSERT(hport != NULL);
  hport->devclass = NULL;
}

/****************************************************************************
 * Name: rk3506_portreset
 ****************************************************************************/

static void rk3506_portreset(struct rk3506_usbhost_s *priv)
{
  uint32_t regval;

  regval  = rk3506_getreg(priv, DWC2_HPRT_OFFSET);
  regval &= ~(DWC2_HPRT_PENA | DWC2_HPRT_PCDET | DWC2_HPRT_PENCHNG |
              DWC2_HPRT_POCCHNG);
  regval |= DWC2_HPRT_PRST;
  rk3506_putreg(priv, DWC2_HPRT_OFFSET, regval);

  up_mdelay(20);

  regval &= ~DWC2_HPRT_PRST;
  rk3506_putreg(priv, DWC2_HPRT_OFFSET, regval);

  up_mdelay(20);
}

/****************************************************************************
 * Name: rk3506_flush_txfifos
 ****************************************************************************/

static void rk3506_flush_txfifos(struct rk3506_usbhost_s *priv,
                                 uint32_t txfnum)
{
  uint32_t regval;
  uint32_t timeout;

  rk3506_putreg(priv, DWC2_GRSTCTL_OFFSET,
                DWC2_GRSTCTL_TXFFLSH | txfnum);

  for (timeout = 0; timeout < RK3506_FLUSH_DELAY; timeout++)
    {
      regval = rk3506_getreg(priv, DWC2_GRSTCTL_OFFSET);
      if ((regval & DWC2_GRSTCTL_TXFFLSH) == 0)
        {
          break;
        }
    }

  up_udelay(3);
}

/****************************************************************************
 * Name: rk3506_flush_rxfifo
 ****************************************************************************/

static void rk3506_flush_rxfifo(struct rk3506_usbhost_s *priv)
{
  uint32_t regval;
  uint32_t timeout;

  rk3506_putreg(priv, DWC2_GRSTCTL_OFFSET, DWC2_GRSTCTL_RXFFLSH);

  for (timeout = 0; timeout < RK3506_FLUSH_DELAY; timeout++)
    {
      regval = rk3506_getreg(priv, DWC2_GRSTCTL_OFFSET);
      if ((regval & DWC2_GRSTCTL_RXFFLSH) == 0)
        {
          break;
        }
    }

  up_udelay(3);
}

/****************************************************************************
 * Name: rk3506_vbusdrive
 ****************************************************************************/

static void rk3506_vbusdrive(struct rk3506_usbhost_s *priv, bool state)
{
  uint32_t regval;

  regval = rk3506_getreg(priv, DWC2_HPRT_OFFSET);
  regval &= ~(DWC2_HPRT_PENA | DWC2_HPRT_PCDET | DWC2_HPRT_PENCHNG |
              DWC2_HPRT_POCCHNG);

  if (((regval & DWC2_HPRT_PPWR) == 0) && state)
    {
      regval |= DWC2_HPRT_PPWR;
      rk3506_putreg(priv, DWC2_HPRT_OFFSET, regval);
    }

  if (((regval & DWC2_HPRT_PPWR) != 0) && !state)
    {
      regval &= ~DWC2_HPRT_PPWR;
      rk3506_putreg(priv, DWC2_HPRT_OFFSET, regval);
    }

  up_mdelay(200);
}

/****************************************************************************
 * Name: rk3506_host_initialize
 ****************************************************************************/

static void rk3506_host_initialize(struct rk3506_usbhost_s *priv)
{
  uint32_t regval;
  uint32_t offset;
  int i;

  rk3506_putreg(priv, DWC2_PCGCCTL_OFFSET, 0);

  regval  = rk3506_getreg(priv, DWC2_HCFG_OFFSET);
  regval &= ~DWC2_HCFG_FSLSPCS_MASK;
  regval |= DWC2_HCFG_FSLSPCS_FS48MHz;
  rk3506_putreg(priv, DWC2_HCFG_OFFSET, regval);

  rk3506_portreset(priv);

  regval = rk3506_getreg(priv, DWC2_HCFG_OFFSET);
  regval &= ~DWC2_HCFG_FSLSS;
  rk3506_putreg(priv, DWC2_HCFG_OFFSET, regval);

  rk3506_putreg(priv, DWC2_GRXFSIZ_OFFSET,
                CONFIG_RK3506_DWC2_RXFIFO_SIZE);
  offset = CONFIG_RK3506_DWC2_RXFIFO_SIZE;

  regval = (offset |
            (CONFIG_RK3506_DWC2_NPTXFIFO_SIZE <<
            DWC2_HNPTXFSIZ_NPTXFD_SHIFT));
  rk3506_putreg(priv, DWC2_HNPTXFSIZ_OFFSET, regval);
  offset += CONFIG_RK3506_DWC2_NPTXFIFO_SIZE;

  regval = (offset |
            (CONFIG_RK3506_DWC2_PTXFIFO_SIZE << 16));
  rk3506_putreg(priv, DWC2_HPTXFSIZ_OFFSET, regval);

  rk3506_flush_txfifos(priv, DWC2_GRSTCTL_TXFNUM_HALL);
  rk3506_flush_rxfifo(priv);

  for (i = 0; i < RK3506_DWC2_NHOST_CHANNELS; i++)
    {
      rk3506_putreg(priv, DWC2_HCINT_OFFSET(i), 0xffffffff);
      rk3506_putreg(priv, DWC2_HCINTMSK_OFFSET(i), 0);
    }

  rk3506_vbusdrive(priv, true);
  rk3506_hostinit_enable(priv);
}

/****************************************************************************
 * Name: rk3506_sw_initialize
 ****************************************************************************/

static inline void rk3506_sw_initialize(struct rk3506_usbhost_s *priv)
{
  struct usbhost_driver_s *drvr;
  struct usbhost_hubport_s *hport;
  int i;

  drvr                 = &priv->drvr;
  drvr->ep0configure   = rk3506_ep0configure;
  drvr->epalloc        = rk3506_epalloc;
  drvr->epfree         = rk3506_epfree;
  drvr->alloc          = rk3506_alloc;
  drvr->free           = rk3506_free;
  drvr->ioalloc        = rk3506_ioalloc;
  drvr->iofree         = rk3506_iofree;
  drvr->ctrlin         = rk3506_ctrlin;
  drvr->ctrlout        = rk3506_ctrlout;
  drvr->transfer       = rk3506_transfer;
  drvr->cancel         = rk3506_cancel;
#ifdef CONFIG_USBHOST_HUB
  drvr->connect        = rk3506_connect;
#endif
  drvr->disconnect     = rk3506_disconnect;

  hport                = &priv->rhport.hport;
  hport->drvr          = drvr;
#ifdef CONFIG_USBHOST_HUB
  hport->parent        = NULL;
#endif
  hport->ep0           = (usbhost_ep_t)&priv->ep0;
  hport->speed         = USB_SPEED_FULL;

  usbhost_devaddr_initialize(&priv->devgen);
  priv->rhport.pdevgen = &priv->devgen;

  priv->smstate   = SMSTATE_DETACHED;
  priv->connected = false;
  priv->change    = false;

  memset(priv->chan, 0,
         RK3506_MAX_TX_FIFOS * sizeof(struct rk3506_chan_s));

  for (i = 0; i < RK3506_MAX_TX_FIFOS; i++)
    {
      struct rk3506_chan_s *chan = &priv->chan[i];
      chan->chidx = i;
      nxsem_init(&chan->waitsem, 0, 0);
    }
}

/****************************************************************************
 * Name: rk3506_usbphy_init
 *
 * Description:
 *   Power up / resume the INNO USB2 PHY port feeding this controller and
 *   apply the SDK tuning.  The PHY comes out of POR in the suspend state
 *   (GRF phy_sus = 0x1d1); with the port suspended the UTMI linestate is
 *   garbage and the DWC2 core reports phantom connect interrupts, which
 *   used to make the usbhost core enumerate a bogus device.
 *
 *   Sequence replicated from Linux drivers/phy/rockchip/
 *   phy-rockchip-inno-usb2.c (rk3506_usb2phy_tuning + clk480m_prepare +
 *   rockchip_usb2phy_power_on).
 *
 ****************************************************************************/

static void rk3506_usbphy_init(struct rk3506_usbhost_s *priv)
{
  bool otg1 = (priv->base != DWC2_OTG0_BASE);
  uintptr_t phybase = RK3506_USB2PHY_ADDR;
  uintptr_t tune;
  uintptr_t lssel;
  uint32_t regval;

  tune = otg1 ? RK3506_USBPHY_OTG1_TUNE : RK3506_USBPHY_OTG0_TUNE;
  lssel = otg1 ? RK3506_USBPHY_OTG1_LSSEL : RK3506_USBPHY_OTG0_LSSEL;

  /* 1. Ungate the PHY reference clock (24M) and this controller's bus
   *    clocks.  Reset default is ungated; this is belt-and-suspenders in
   *    case the previous boot stage closed them.  Rockchip gate bits are
   *    active-high "clock stopped", written with write-enable in the
   *    high half-word: writing value 0 with WE keeps the clock running.
   */

  putreg32(RK3506_CON1_REF_USBPHY << 16, RK3506_CRU_CLKGATE_CON(1));
  regval = (otg1 ? RK3506_CON7_OTG1_BITS : RK3506_CON7_OTG0_BITS) << 16;
  putreg32(regval, RK3506_CRU_CLKGATE_CON(7));

  /* 2. Turn on the PHY 480MHz clock output (clkout_ctl_phy, bits [7:2] =
   *    0x27) and wait for it to stabilize.
   */

  regval  = getreg32(phybase + RK3506_USBPHY_CLKOUT_CTL);
  regval &= ~(0x3fu << 2);
  regval |=  (0x27u << 2);
  putreg32(regval, phybase + RK3506_USBPHY_CLKOUT_CTL);
  up_udelay(1300);

  /* 3. Port tuning (rk3506_usb2phy_tuning): turn off the differential
   *    receiver in suspend mode, set the HS eye height to 425mV and
   *    source the fs/ls linestate from the TX driver.
   */

  regval  = getreg32(phybase + tune);
  regval &= ~(1u << 2);            /* diff receiver off in suspend */
  regval &= ~(0x7u << 4);          /* HS eye height bits [6:4] */
  regval |=  (0x5u << 4);          /* 425mV */
  putreg32(regval, phybase + tune);

  regval  = getreg32(phybase + lssel);
  regval &= ~(0xfu << 3);          /* linestate select bits [6:3] */
  regval |=  (0x3u << 3);          /* from TX driver */
  putreg32(regval, phybase + lssel);

  /* 4. Take the port out of suspend: GRF phy_sus bits [8:0] = 0 with
   *    write-enable mask in the high half-word, then wait for the UTMI
   *    interface clock to become stable.
   */

  regval = RK3506_GRF_PHY_SUS_MASK << 16;
  putreg32(regval, RK3506_GRF_ADDR +
                   (otg1 ? RK3506_GRF_SOC_CON28 : RK3506_GRF_SOC_CON24));
  up_udelay(2000);
}

/****************************************************************************
 * Name: rk3506_hw_initialize
 ****************************************************************************/

static inline int rk3506_hw_initialize(struct rk3506_usbhost_s *priv)
{
  uint32_t regval;
  unsigned long timeout;

  /* Resume the shared INNO USB2 PHY port for this controller first: it
   * comes out of POR suspended and without this the UTMI linestate is
   * garbage (phantom connects). */

  rk3506_usbphy_init(priv);

  regval = rk3506_getreg(priv, DWC2_GUSBCFG_OFFSET);
  regval |= DWC2_GUSBCFG_PHYSEL;
  rk3506_putreg(priv, DWC2_GUSBCFG_OFFSET, regval);

  for (timeout = 0; timeout < RK3506_READY_DELAY; timeout++)
    {
      up_udelay(3);
      regval = rk3506_getreg(priv, DWC2_GRSTCTL_OFFSET);
      if ((regval & DWC2_GRSTCTL_AHBIDL) != 0)
        {
          break;
        }
    }

  if (timeout >= RK3506_READY_DELAY)
    {
      uerr("ERROR: AHB idle timeout\n");
      return -ETIMEDOUT;
    }

  rk3506_putreg(priv, DWC2_GRSTCTL_OFFSET, DWC2_GRSTCTL_CSRST);
  for (timeout = 0; timeout < RK3506_READY_DELAY; timeout++)
    {
      regval = rk3506_getreg(priv, DWC2_GRSTCTL_OFFSET);
      if ((regval & DWC2_GRSTCTL_CSRST) == 0)
        {
          break;
        }
    }

  if (timeout >= RK3506_READY_DELAY)
    {
      uerr("ERROR: Core reset timeout\n");
      return -ETIMEDOUT;
    }

  up_udelay(3);

  regval = DWC2_GCCFG_PWRDWN | DWC2_GCCFG_VBUSBSEN |
           DWC2_GCCFG_NOVBUSSENS;
  rk3506_putreg(priv, DWC2_GCCFG_OFFSET, regval);
  up_mdelay(20);

  regval  = rk3506_getreg(priv, DWC2_GUSBCFG_OFFSET);
  regval &= ~DWC2_GUSBCFG_FDMOD;
  regval |= DWC2_GUSBCFG_FHMOD;
  rk3506_putreg(priv, DWC2_GUSBCFG_OFFSET, regval);
  up_mdelay(50);

  rk3506_host_initialize(priv);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_usbhost_initialize
 ****************************************************************************/

struct usbhost_connection_s *rk3506_usbhost_initialize(int controller)
{
  struct rk3506_usbhost_s *priv;
  int irq;

  if (controller == 0)
    {
      priv = &g_usbhost0;
      priv->base = DWC2_OTG0_BASE;
      priv->irq  = DWC2_OTG0_IRQ;
    }
#ifdef CONFIG_RK3506_USBHOST_OTG1
  else if (controller == 1)
    {
      priv = &g_usbhost1;
      priv->base = RK3506_USBOTG1_ADDR;
      priv->irq  = RK3506_IRQ_USBOTG1;
    }
#endif
  else
    {
      uerr("ERROR: Invalid controller: %d\n", controller);
      return NULL;
    }

  irq = priv->irq;

  rk3506_gint_disable(priv);

  rk3506_sw_initialize(priv);

  if (rk3506_hw_initialize(priv) < 0)
    {
      int i;
      uerr("ERROR: Hardware initialization failed\n");
      /* Destroy semaphores initialized in rk3506_sw_initialize */

      for (i = 0; i < RK3506_MAX_TX_FIFOS; i++)
        {
          nxsem_destroy(&priv->chan[i].waitsem);
        }

      return NULL;
    }

  if (irq_attach(irq, rk3506_gint_isr, priv) != 0)
    {
      int i;
      uerr("ERROR: irq_attach failed for IRQ %d\n", irq);
      for (i = 0; i < RK3506_MAX_TX_FIFOS; i++)
        {
          nxsem_destroy(&priv->chan[i].waitsem);
        }
      return NULL;
    }

  rk3506_gint_enable(priv);

  up_enable_irq(irq);

  return (struct usbhost_connection_s *)&priv->conn;
}

/****************************************************************************
 * Name: usbhost_trformat1 / usbhost_trformat2
 *
 * Description:
 *   Trace-ID -> format-string hooks required by drivers/usbhost/
 *   usbhost_trace.c when CONFIG_USBHOST_TRACE or CONFIG_DEBUG_USB is
 *   enabled (every HCD provides these; see e.g. stm32_usbhost.c).
 *
 *   This driver does not emit USB trace events, so a NULL return is
 *   correct: the trace functions are then no-ops.
 *
 ****************************************************************************/

#if defined(CONFIG_USBHOST_TRACE) || \
   (defined(CONFIG_DEBUG_FEATURES) && defined(CONFIG_DEBUG_USB))

FAR const char *usbhost_trformat1(uint16_t id)
{
  return NULL;
}

FAR const char *usbhost_trformat2(uint16_t id)
{
  return NULL;
}

#endif /* CONFIG_USBHOST_TRACE || (DEBUG_FEATURES && DEBUG_USB) */

#endif /* CONFIG_RK3506_USBHOST */
