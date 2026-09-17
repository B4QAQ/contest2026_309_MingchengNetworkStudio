/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_can.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CANFD controller driver for the RK3506G2 (Rockchip CANFD IP, the same
 * block as rk3576), NuttX SocketCAN (netdev_lowerhalf_s) integration.
 *
 * Register knowledge base (Linux SDK, authoritative):
 *  - kernel-6.1/drivers/net/can/rockchip/rk3576_canfd.c - the driver the
 *    "rockchip,rk3506-canfd"/"rockchip,rk3576-canfd" compatible binds to
 *    (u-boot/arch/arm/dts/rk3506.dtsi lines 783-805: can0 @0xFF320000,
 *    can1 @0xFF330000, GIC SPI 45/46).
 *  - hal/lib/hal/src/hal_canfd.c (SOC_RK3506 variant) cross-checks the
 *    INT/poll layouts.
 *
 * Implemented (classic CAN 2.0, no FD, one TX slot, quota model):
 *  - ifup:  reset pulse -> INT_MASK -> ATF accept-all -> STR_CTL/WTM
 *           (internal SRAM fixed 18-word frames, watermark 0x7e) ->
 *           loopback/error mask -> AUTO_RETX -> FD_BRS_CFG=0x7 ->
 *           bus-off recovery -> NBTP -> WORK_MODE.
 *  - TX:    TXID/TXFIC/TXDATA + CMD=TX0_REQ (kernel bit layout);
 *           TX_FINISH -> CMD=0, txconfirm via receive() (CAN_TCF).
 *  - RX:    stream FIFO @0x400, 18 words per frame (info, id, 16 data);
 *           watermark/timeout/full interrupts -> rxready; receive()
 *           drains STR_STATE.LEFT_CNT/18 frames.
 *  - Loopback (CONFIG_RK3506_CAN_LOOPBACK): MODE_LBACK + mask ACK
 *    errors, per the kernel CAN_CTRLMODE_LOOPBACK path - a single node
 *    can self-send/self-receive without a second node or transceiver.
 *
 * Clock/reset (clk-rk3506.c): CLK_CAN0/1 = gpll/6 = 200 MHz (the rate
 * the HAL bit-timing tables are calibrated for), HCLK gates + SRST in
 * CLKGATE_CON(13)/SOFTRST_CON(13) bits 4..7.
 *
 * Pins: RM_IO11 -> CAN0 (TX func 43 / RX func 44), RM_IO12 -> CAN1
 * (TX func 41 / RX func 42).  Any RM_IO pin can take any function ID
 * (RM_IO matrix); CAN is kept off RM_IO10 so it does not conflict with
 * PWM0_CH2 (f47).
 *
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/can.h>

#include "rk3506_cru.h"

#ifdef CONFIG_RK3506_CAN

#ifndef putreg32
#  define putreg32(v, a) (*(FAR volatile uint32_t *)(a) = (v))
#endif
#ifndef getreg32
#  define getreg32(a)    (*(FAR volatile uint32_t *)(a))
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAN0_BASE               0xFF320000u
#define CAN1_BASE               0xFF330000u

#define CAN_IRQ0                RK3506_IRQ_CAN0
#define CAN_IRQ1                RK3506_IRQ_CAN1

/* Register offsets (rk3576_canfd.c enum / rk3506.h CAN_REG) */

#define CAN_MODE                0x000
#define CAN_CMD                 0x004
#define CAN_STATE               0x008
#define CAN_INT                 0x00c
#define CAN_INT_MASK            0x010
#define CAN_NBTP                0x100
#define CAN_BRS_CFG             0x10c
#define CAN_TXFIC               0x200
#define CAN_TXID                0x204
#define CAN_TXDATA              0x208
#define CAN_RX_FIFO_RDATA       0x400
#define CAN_STR_CTL             0x600
#define CAN_STR_STATE           0x604
#define CAN_STR_WTM             0x60c
#define CAN_ATF                 0x700
#define CAN_ATFM                0x714
#define CAN_AUTO_RETX_CFG       0x808
#define CAN_BUSOFF_RCY_CFG      0x830
#define CAN_BUSOFF_RCY_THR      0x834
#define CAN_ERROR_MASK          0x904

/* MODE bits */

#define MODE_WORK               (1u << 0)
#define MODE_LBACK              (1u << 4)

/* CMD bits */

#define CMD_TX0_REQ             (1u << 0)

/* INT bits (1 = masked in INT_MASK) */

#define INT_RX_FINISH           (1u << 0)
#define INT_TX_FINISH           (1u << 1)
#define INT_RXSTR_FULL          (1u << 7)
#define INT_RXSTR_TIMEOUT       (1u << 15)
#define INT_ISM_WTM             (1u << 17)
#define INT_BUSOFF              (1u << 9)

/* Enabled RX interrupts: watermark + storage timeout + FIFO full */

#define CAN_RX_IRQS             (INT_ISM_WTM | INT_RXSTR_TIMEOUT | \
                                 INT_RXSTR_FULL)

/* TX frame info (TXFIC) */

#define TXFIC_DLC_MASK          0xfu
#define TXFIC_BRS               (1u << 4)
#define TXFIC_FDF               (1u << 5)
#define TXFIC_RTR               (1u << 6)
#define TXFIC_IDE               (1u << 7)

/* RX frame info (first FIFO word) */

#define RXFIC_DLC_SHIFT         24
#define RXFIC_DLC_MASK          (0xfu << RXFIC_DLC_SHIFT)
#define RXFIC_BRS               (1u << 20)
#define RXFIC_FDF               (1u << 21)
#define RXFIC_RTR               (1u << 22)
#define RXFIC_IDE               (1u << 23)

/* STR_STATE: bits [16:8] = words left in the RX SRAM */

#define STR_LEFTCNT_SHIFT       8
#define STR_LEFTCNT_MASK        (0x1ffu << STR_LEFTCNT_SHIFT)

/* Internal SRAM storage: fixed 18-word frames (rx-max-data default),
 * watermark 0x7e words (kernel ISM_WATERMASK_CANFD) with ISM_SEL = 2
 * (CANFD_DATA_CANFD_FIXED) and the storage timeout mode enabled.
 */

#define CANFD_RX_MAX_DATA       18
#define STR_ISM_CANFD_FIXED     2u
#define STR_ISM_SEL_SHIFT       2
#define STR_STORAGE_TIMEOUT     (1u << 8)
#define STR_WTM_CANFD           0x7eu

/* AUTO retransmission: enabled, limit 300 (kernel) */

#define AUTO_RETX_EN            (1u << 0)
#define AUTO_RETX_LIMIT_EN      (1u << 1)
#define AUTO_RETX_LIMIT_SHIFT   3
#define AUTO_RETX_LIMIT_CNT     0x12c

/* Bus-off recovery: fast counters (kernel) */

#define BUSOFF_RCY_EN           (1u << 8)
#define BUSOFF_RCY_CNT_FAST     4
#define BUSOFF_RCY_TIME_FAST    0x3d0900

/* Error mask: mask the ACK error bit in loopback mode */

#define ERROR_ACK               (1u << 4)

/* ATF accept-all: code 0, mask 0x7fff (kernel CANFD_ATF_MASK_MODE) */

#define ATF_MASK_ALL            0x7fffu

/* Bit timing register layout: NSJW[30:24], NBRP[23:16],
 * NTSEG2[14:8], NTSEG1[7:0] (register fields, not TQ counts).
 */

#define NBTP_SJW_SHIFT          24
#define NBTP_BRQ_SHIFT          16
#define NBTP_TSEG2_SHIFT        8
#define NBTP_TSEG1_SHIFT        0

/* Clock */

#define CAN_CLK_HZ              200000000u   /* gpll 1.2 GHz / 6 */

/* Bit timing table for CAN_CLK_HZ = 200 MHz, ported from
 * hal_canfd.c HAL_CANFD_SetNBps (brq / tseg1 / tseg2 register values).
 */

struct rk3506_can_bittime_s
{
  uint32_t bitrate;
  uint32_t brq;
  uint32_t tseg1;
  uint32_t tseg2;
};

static const struct rk3506_can_bittime_s g_can_bittime[] =
{
  { 1000000,  4, 13, 4 },
  {  500000,  4, 33, 4 },
  {  250000,  4, 68, 9 },
  {  125000,  9, 68, 9 },
};

/* Pins: RM_IO routing register data (func - 15) and IOMUX nibble = 7.
 * CAN0 on RM_IO11 (GPIO0_B3): TX f43 / RX f44.
 * CAN1 on RM_IO12 (GPIO0_C0): TX f41 / RX f42.
 */

#define RMIO_REG(pin)           (0xFF910000u + 0x80u + 0x4u * (pin))
#define RMIO_DATA(fn)           (0x7f0000u | ((fn) & 0x7fu))

#define CAN0_RMIOPIN            11
#define CAN0_TX_FN              (43u - 15u)
#define CAN0_RX_FN              (44u - 15u)

#define CAN1_RMIOPIN            12
#define CAN1_TX_FN              (41u - 15u)
#define CAN1_RX_FN              (42u - 15u)

/* GPIO0 bank IOMUX: pin 11 -> GPIO0B3 (reg 0x08 nibble 3),
 * pin 12 -> GPIO0C0 (reg 0x10 nibble 0).
 */

#define IOC_GPIO0_SEL(n)        (0xFF950000u + 0x8u * (n))
#define CAN0_IOMUX_REG          IOC_GPIO0_SEL(1)
#define CAN0_IOMUX_BIT          12         /* nibble 3 -> bits 12..15 */
#define CAN1_IOMUX_REG          IOC_GPIO0_SEL(2)
#define CAN1_IOMUX_BIT          0          /* nibble 0 -> bits 0..3 */

#define CAN_IOMUX_FUNC          7u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_can_s
{
  struct netdev_lowerhalf_s dev;        /* Must be first */
  uint32_t base;                        /* Controller register base */
  uint32_t bitrate;                     /* Configured bitrate */
  int irq;
  uint8_t rmiopin;
  uint32_t tx_fn;                       /* RM_IO routing function, TX */
  uint32_t rx_fn;                       /* RM_IO routing function, RX */
  uint32_t iomux_bit;                   /* GPIO0 iomux nibble bit */
  uint32_t iomux_reg;
  volatile bool rx_pending;             /* ISR saw RX watermark/timeout */
  FAR netpkt_t *tx_pkt;                 /* In-flight TX pkt (single slot) */
  bool tx_confirm;                      /* TX_FINISH seen, confirm pending */
  bool up;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int can_ifup(FAR struct netdev_lowerhalf_s *dev);
static int can_ifdown(FAR struct netdev_lowerhalf_s *dev);
static int can_transmit(FAR struct netdev_lowerhalf_s *dev,
                        FAR netpkt_t *pkt);
static FAR netpkt_t *can_receive(FAR struct netdev_lowerhalf_s *dev);
#ifdef CONFIG_NETDEV_IOCTL
static int can_ioctl(FAR struct netdev_lowerhalf_s *dev, int cmd,
                     unsigned long arg);
#endif
static int can_interrupt(int irq, FAR void *context, FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct netdev_ops_s g_can_ops =
{
  .ifup     = can_ifup,
  .ifdown   = can_ifdown,
  .transmit = can_transmit,
  .receive  = can_receive,
#ifdef CONFIG_NETDEV_IOCTL
  .ioctl    = can_ioctl,
#endif
};

static struct rk3506_can_s g_can[2];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: can_set_reset_mode
 *
 * Description:
 *   Enter reset mode: pulse the CAN SRST and clear MODE
 *   (kernel set_reset_mode).
 *
 ****************************************************************************/

static void can_set_reset_mode(FAR struct rk3506_can_s *priv)
{
  uint32_t bit = (priv->base == CAN0_BASE) ? (1u << 5) : (1u << 7);

  /* SRST_CAN0 = CON(13) bit 5, SRST_CAN1 = CON(13) bit 7 */

  putreg32(bit << 16 | bit, 0xff480000u + 13u * 4u + 0xa00u);
  up_udelay(2);
  putreg32(bit << 16, 0xff480000u + 13u * 4u + 0xa00u);

  putreg32(0, priv->base + CAN_MODE);
}

/****************************************************************************
 * Name: can_nbtp
 *
 * Description:
 *   Build the nominal bit timing register value for priv->bitrate.
 *
 ****************************************************************************/

static uint32_t can_nbtp(FAR struct rk3506_can_s *priv)
{
  FAR const struct rk3506_can_bittime_s *bt = &g_can_bittime[1];
  unsigned int i;

  for (i = 0; i < nitems(g_can_bittime); i++)
    {
      if (g_can_bittime[i].bitrate == priv->bitrate)
        {
          bt = &g_can_bittime[i];
          break;
        }
    }

  return (0u << NBTP_SJW_SHIFT) |
         (bt->brq << NBTP_BRQ_SHIFT) |
         (bt->tseg2 << NBTP_TSEG2_SHIFT) |
         (bt->tseg1 << NBTP_TSEG1_SHIFT);
}

/****************************************************************************
 * Name: can_ifup
 *
 * Description:
 *   Start the controller (kernel rk3576_canfd_start, classic CAN).
 *
 ****************************************************************************/

static int can_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct rk3506_can_s *priv = (FAR struct rk3506_can_s *)dev;
  uint32_t val;

  if (priv->up)
    {
      return OK;
    }

  /* Pins: RM_IO routing functions + IOMUX nibbles = 7 */

  putreg32(RMIO_DATA(priv->tx_fn), RMIO_REG(priv->rmiopin));
  putreg32(RMIO_DATA(priv->rx_fn), RMIO_REG(priv->rmiopin));
  putreg32((0xfu << (priv->iomux_bit + 16)) |
           (CAN_IOMUX_FUNC << priv->iomux_bit), priv->iomux_reg);

  /* Clocks */

  rk3506_can_clock_init(priv->base == CAN0_BASE ? 0 : 1);

  /* Reset mode + register configuration */

  can_set_reset_mode(priv);

  /* Mask everything except the RX watermark/timeout/full set and
   * TX_FINISH (kernel: INT_ENABLE masks RX_FINISH only, leaving the
   * stream-mode interrupts enabled).
   */

  putreg32(0xffffu & ~(CAN_RX_IRQS | INT_TX_FINISH),
           priv->base + CAN_INT_MASK);

  /* Accept-all hardware filters (kernel ATF mask mode) */

  for (val = 0; val < 5; val++)
    {
      putreg32(0, priv->base + CAN_ATF + 4u * val);
      putreg32(ATF_MASK_ALL, priv->base + CAN_ATFM + 4u * val);
    }

  /* Internal SRAM stream storage: fixed 18-word frames, watermark
   * 0x7e words, storage timeout mode (kernel).
   */

  putreg32((STR_ISM_CANFD_FIXED << STR_ISM_SEL_SHIFT) |
           STR_STORAGE_TIMEOUT, priv->base + CAN_STR_CTL);
  putreg32(STR_WTM_CANFD, priv->base + CAN_STR_WTM);

  /* Loopback mode: internal loopback + mask ACK errors, so a single
   * node can transmit without a second node on the bus (kernel
   * CAN_CTRLMODE_LOOPBACK path).
   */

  val = getreg32(priv->base + CAN_MODE);
#ifdef CONFIG_RK3506_CAN_LOOPBACK
  val |= MODE_LBACK;
  putreg32(ERROR_ACK, priv->base + CAN_ERROR_MASK);
#else
  putreg32(0, priv->base + CAN_ERROR_MASK);
#endif

  /* Auto retransmission with 300-try limit, BRS config, bus-off
   * recovery (fast counters, 40 ms threshold) - all per kernel.
   */

  putreg32(AUTO_RETX_EN | AUTO_RETX_LIMIT_EN |
           (AUTO_RETX_LIMIT_CNT << AUTO_RETX_LIMIT_SHIFT),
           priv->base + CAN_AUTO_RETX_CFG);
  putreg32(0x7, priv->base + CAN_BRS_CFG);
  putreg32(BUSOFF_RCY_EN | BUSOFF_RCY_CNT_FAST,
           priv->base + CAN_BUSOFF_RCY_CFG);
  putreg32(BUSOFF_RCY_TIME_FAST, priv->base + CAN_BUSOFF_RCY_THR);

  /* Bit timing + start */

  putreg32(can_nbtp(priv), priv->base + CAN_NBTP);
  putreg32(val | MODE_WORK, priv->base + CAN_MODE);

  priv->rx_pending = false;
  priv->up         = true;

  /* Interrupts */

  irq_attach(priv->irq, can_interrupt, priv);
  up_enable_irq(priv->irq);

  netdev_lower_carrier_on(dev);

  caninfo("can%u up: base=0x%08" PRIx32 " bitrate=%" PRIu32
          " NBTP=0x%08" PRIx32 " loopback=%d\n",
          priv->base == CAN0_BASE ? 0u : 1u,
          priv->base, priv->bitrate, can_nbtp(priv),
#ifdef CONFIG_RK3506_CAN_LOOPBACK
          1
#else
          0
#endif
          );
  return OK;
}

/****************************************************************************
 * Name: can_ifdown
 *
 * Description:
 *   Stop the controller (kernel rk3576_canfd_stop).
 *
 ****************************************************************************/

static int can_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct rk3506_can_s *priv = (FAR struct rk3506_can_s *)dev;

  if (!priv->up)
    {
      return OK;
    }

  netdev_lower_carrier_off(dev);
  up_disable_irq(priv->irq);

  can_set_reset_mode(priv);
  putreg32(0xffffffffu, priv->base + CAN_INT_MASK);

  /* Release a pending TX pkt (quota hygiene) */

  if (priv->tx_pkt != NULL)
    {
      netpkt_free(dev, priv->tx_pkt, NETPKT_TX);
      priv->tx_pkt = NULL;
      atomic_add(&dev->quota_ptr[NETPKT_TX], 1);
    }

  priv->tx_confirm = false;
  priv->rx_pending = false;
  priv->up         = false;
  return OK;
}

/****************************************************************************
 * Name: can_transmit
 *
 * Description:
 *   Write one classic CAN frame into the TX slot and request
 *   transmission (kernel rk3576_canfd_start_xmit, 1 in-flight frame).
 *
 ****************************************************************************/

static int can_transmit(FAR struct netdev_lowerhalf_s *dev,
                        FAR netpkt_t *pkt)
{
  FAR struct rk3506_can_s *priv = (FAR struct rk3506_can_s *)dev;
  FAR struct can_frame *frame =
    (FAR struct can_frame *)netpkt_getdata(dev, pkt);
  uint32_t id;
  uint32_t fic;
  uint32_t len;
  uint32_t i;

  if (frame->can_id & CAN_EFF_FLAG)
    {
      id  = frame->can_id & CAN_EFF_MASK;
      fic = TXFIC_IDE;
    }
  else
    {
      id  = frame->can_id & CAN_SFF_MASK;
      fic = 0;
    }

  if (frame->can_id & CAN_RTR_FLAG)
    {
      fic |= TXFIC_RTR;
    }

  len = frame->can_dlc;
  if (len > 8)
    {
      len = 8;
    }

  /* DLC is the plain length for classic CAN (kernel: can_fd_len2dlc
   * for FD frames; for classic frames len2dlc identity <= 8).
   */

  fic |= (len & TXFIC_DLC_MASK);

  putreg32(id, priv->base + CAN_TXID);
  putreg32(fic, priv->base + CAN_TXFIC);

  for (i = 0; i < len; i += 4)
    {
      uint32_t word = 0;
      memcpy(&word, &frame->data[i], len - i < 4 ? len - i : 4);
      putreg32(word, priv->base + CAN_TXDATA + i);
    }

  priv->tx_pkt = pkt;

  putreg32(CMD_TX0_REQ, priv->base + CAN_CMD);
  return OK;
}

/****************************************************************************
 * Name: can_pop_frame
 *
 * Description:
 *   Pop one frame from the RX stream FIFO (18 words: info, id, then
 *   16 data words - kernel rk3576_canfd_rx) and copy it into a fresh
 *   RX pkt.
 *
 ****************************************************************************/

static FAR netpkt_t *can_pop_frame(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct rk3506_can_s *priv = (FAR struct rk3506_can_s *)dev;
  FAR netpkt_t *pkt;
  FAR struct can_frame *frame;
  uint32_t info;
  uint32_t id;
  uint32_t data[16];
  uint32_t len;
  uint32_t i;

  info = getreg32(priv->base + CAN_RX_FIFO_RDATA);
  id   = getreg32(priv->base + CAN_RX_FIFO_RDATA);

  for (i = 0; i < CANFD_RX_MAX_DATA - 2; i++)
    {
      data[i] = getreg32(priv->base + CAN_RX_FIFO_RDATA);
    }

  if (info & RXFIC_FDF)
    {
      /* FD frames are dropped: stack runs classic CAN only */

      canwarn("CAN FD frame dropped (no FD support)\n");
      return NULL;
    }

  len = (info & RXFIC_DLC_MASK) >> RXFIC_DLC_SHIFT;
  if (len > 8)
    {
      len = 8;
    }

  pkt = netpkt_alloc(dev, NETPKT_RX);
  if (pkt == NULL)
    {
      canerr("RX pkt alloc failed\n");
      return NULL;
    }

  netpkt_setdatalen(dev, pkt, sizeof(struct can_frame));
  frame = (FAR struct can_frame *)netpkt_getdata(dev, pkt);
  memset(frame, 0, sizeof(*frame));

  if (info & RXFIC_IDE)
    {
      frame->can_id = id & CAN_EFF_MASK;
      frame->can_id |= CAN_EFF_FLAG;
    }
  else
    {
      frame->can_id = id & CAN_SFF_MASK;
    }

  if (info & RXFIC_RTR)
    {
      frame->can_id |= CAN_RTR_FLAG;
    }

  frame->can_dlc = len;
  for (i = 0; i < len; i += 4)
    {
      uint32_t word = data[i / 4];
      uint32_t n = len - i < 4 ? len - i : 4;
      memcpy(&frame->data[i], &word, n);
    }

  return pkt;
}

/****************************************************************************
 * Name: can_txconfirm
 *
 * Description:
 *   Return the in-flight TX pkt with the CAN_TCF flag (the echo
 *   confirm), restoring TX quota (ctucanfd_sock_txconfirm pattern).
 *
 ****************************************************************************/

#ifdef CONFIG_NET_CAN_TXCONFIRM
static FAR netpkt_t *can_txconfirm(FAR struct rk3506_can_s *priv)
{
  FAR struct netdev_lowerhalf_s *dev = &priv->dev;
  FAR netpkt_t *pkt;

  if (!priv->tx_confirm || priv->tx_pkt == NULL)
    {
      return NULL;
    }

  if (atomic_sub(&dev->quota_ptr[NETPKT_RX], 1) <= 0)
    {
      atomic_add(&dev->quota_ptr[NETPKT_RX], 1);
      return NULL;
    }

  pkt = priv->tx_pkt;
  priv->tx_pkt     = NULL;
  priv->tx_confirm = false;

  ((FAR struct can_frame *)netpkt_getdata(dev, pkt))->flags = CAN_TCF;

  atomic_add(&dev->quota_ptr[NETPKT_TX], 1);
  return pkt;
}
#endif

/****************************************************************************
 * Name: can_receive
 *
 * Description:
 *   Drain pending RX frames, then hand back the TX confirm pkt if
 *   any (dual-duty receive, SocketCAN lower-half pattern).
 *
 ****************************************************************************/

static FAR netpkt_t *can_receive(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct rk3506_can_s *priv = (FAR struct rk3506_can_s *)dev;
  FAR netpkt_t *pkt;
  uint32_t words;

  /* Frames waiting in the internal SRAM? */

  words = (getreg32(priv->base + CAN_STR_STATE) & STR_LEFTCNT_MASK)
          >> STR_LEFTCNT_SHIFT;

  if (words >= CANFD_RX_MAX_DATA)
    {
      pkt = can_pop_frame(dev);
      if (pkt != NULL)
        {
          return pkt;
        }
    }

#ifdef CONFIG_NET_CAN_TXCONFIRM
  pkt = can_txconfirm(priv);
  if (pkt != NULL)
    {
      return pkt;
    }
#else
  if (priv->tx_confirm && priv->tx_pkt != NULL)
    {
      /* Confirmations not requested by the stack: free the pkt and
       * restore quota.
       */

      netpkt_free(dev, priv->tx_pkt, NETPKT_TX);
      priv->tx_pkt     = NULL;
      priv->tx_confirm = false;
      atomic_add(&dev->quota_ptr[NETPKT_TX], 1);
    }
#endif

  if (words < CANFD_RX_MAX_DATA)
    {
      priv->rx_pending = false;
    }

  return NULL;
}

/****************************************************************************
 * Name: can_interrupt
 *
 * Description:
 *   Controller interrupt: TX_FINISH releases the TX slot (kernel:
 *   CMD=0 + wake), RX stream events flag pending frames.
 *
 ****************************************************************************/

static int can_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3506_can_s *priv = (FAR struct rk3506_can_s *)arg;
  uint32_t isr;

  isr = getreg32(priv->base + CAN_INT);

  if (isr & INT_TX_FINISH)
    {
      putreg32(0, priv->base + CAN_CMD);
      priv->tx_confirm = true;
    }

  if (isr & (CAN_RX_IRQS | INT_BUSOFF))
    {
      priv->rx_pending = true;
    }

  if (isr != 0)
    {
      /* Clear the handled interrupts (w1c) */

      putreg32(isr, priv->base + CAN_INT);

      netdev_lower_rxready(&priv->dev);

      if ((isr & INT_TX_FINISH) != 0)
        {
          netdev_lower_txdone(&priv->dev);
        }
    }

  return OK;
}

#ifdef CONFIG_NETDEV_IOCTL
/****************************************************************************
 * Name: can_ioctl
 *
 * Description:
 *   No chip-specific ioctls yet (bitrate via defconfig).
 *
 ****************************************************************************/

static int can_ioctl(FAR struct netdev_lowerhalf_s *dev, int cmd,
                     unsigned long arg)
{
  return -ENOTTY;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_can_initialize
 *
 * Description:
 *   Register both SocketCAN devices (can0 @0xFF320000, can1
 *   @0xFF330000) with the network stack.
 *
 * Returned Value:
 *   Zero on success; negated errno on failure.
 *
 ****************************************************************************/

int rk3506_can_initialize(void)
{
  FAR struct rk3506_can_s *priv;
  int ret;
  int i;

  for (i = 0; i < 2; i++)
    {
      priv = &g_can[i];

      memset(priv, 0, sizeof(*priv));
      priv->dev.ops = &g_can_ops;
      priv->dev.rxtype = NETDEV_RX_THREAD;

      /* Single in-flight TX frame; RX processed frame by frame */

      priv->dev.quota[NETPKT_TX] = 1;
      priv->dev.quota[NETPKT_RX] = 1;

      /* SocketCAN default: echo locally sent frames back to sockets */

      IFF_SET_LOOPBACK(priv->dev.netdev.d_flags);

      snprintf(priv->dev.netdev.d_ifname, IFNAMSIZ, "can%d", i);

      if (i == 0)
        {
          priv->base       = CAN0_BASE;
          priv->irq        = CAN_IRQ0;
          priv->bitrate    = CONFIG_RK3506_CAN0_BITRATE;
          priv->rmiopin    = CAN0_RMIOPIN;
          priv->tx_fn      = CAN0_TX_FN;
          priv->rx_fn      = CAN0_RX_FN;
          priv->iomux_reg  = CAN0_IOMUX_REG;
          priv->iomux_bit  = CAN0_IOMUX_BIT;
        }
      else
        {
          priv->base       = CAN1_BASE;
          priv->irq        = CAN_IRQ1;
          priv->bitrate    = CONFIG_RK3506_CAN1_BITRATE;
          priv->rmiopin    = CAN1_RMIOPIN;
          priv->tx_fn      = CAN1_TX_FN;
          priv->rx_fn      = CAN1_RX_FN;
          priv->iomux_reg  = CAN1_IOMUX_REG;
          priv->iomux_bit  = CAN1_IOMUX_BIT;
        }

      ret = netdev_lower_register(&priv->dev, NET_LL_CAN);
      if (ret < 0)
        {
          canerr("can%d register failed: %d\n", i, ret);
          return ret;
        }
    }

  return OK;
}
#endif /* CONFIG_RK3506_CAN */
