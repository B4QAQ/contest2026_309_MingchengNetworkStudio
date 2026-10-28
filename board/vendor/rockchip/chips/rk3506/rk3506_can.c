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
 *  - ifup:  bitrate table lookup (unknown rate fails ifup explicitly) ->
 *           reset pulse -> INT_MASK (32-bit explicit mask) -> ATF
 *           accept-all -> STR_CTL/WTM (internal SRAM fixed 18-word
 *           frames, watermark 0x7e) -> loopback/error mask -> AUTO_RETX
 *           -> FD_BRS_CFG=0x7 -> bus-off recovery -> NBTP -> WORK_MODE.
 *  - TX:    all validation happens BEFORE the hardware slot / upper TX
 *           quota is consumed; TXID/TXFIC/TXDATA + CMD=TX0_REQ (kernel
 *           bit layout); TX_FINISH -> CMD=0, slot released in receive()
 *           work context (CAN_TCF echo in normal mode, silent free in
 *           hardware loopback mode).
 *  - RX:    stream FIFO @0x400, 18 words per frame (info, id, 16 data);
 *           watermark/timeout/full interrupts -> rxready; receive()
 *           drains STR_STATE.LEFT_CNT/18 frames.
 *  - Loopback (CONFIG_RK3506_CAN_LOOPBACK): MODE_LBACK + mask ACK
 *    errors, per the kernel CAN_CTRLMODE_LOOPBACK path - a single node
 *    can self-send/self-receive without a second node or transceiver.
 *    The hardware loopback frame is the single SocketCAN echo, so the
 *    CAN_TCF software echo is suppressed in this build (one cansend ==
 *    one candump line).
 *
 * Clock/reset (clk-rk3506.c): CLK_CAN0/1 = gpll/6 = 200 MHz (the rate
 * the HAL bit-timing tables are calibrated for), HCLK gates + SRST in
 * CLKGATE_CON(13)/SOFTRST_CON(13) bits 4..7.
 *
 * Pins: the RM_IO matrix can route CAN1 TX/RX (functions 41/42) and
 * CAN0 TX/RX (functions 43/44, see
 * u-boot/arch/arm/dts/rk3506-pinctrl-rmio.dtsi) to ANY RMIO-capable
 * pad.  TX and RX need TWO DIFFERENT pads; there is no board schematic
 * in this project, so the pads are intentionally left unrouted - see
 * can_pinmux() / can_rmio_route() and the TODO(boards) there.  The
 * loopback bench build never touches the pad mux.
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
#include "rk3506_pinctrl.h"
#include "rk3506_register.h"
#include "hardware/rk3506_memorymap.h"

#ifdef CONFIG_RK3506_CAN

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller bases: RK3506_CAN0_ADDR / RK3506_CAN1_ADDR from
 * hardware/rk3506_memorymap.h (single shared address table).
 */

#define CAN0_BASE               RK3506_CAN0_ADDR
#define CAN1_BASE               RK3506_CAN1_ADDR

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
#define CAN_ERROR_CODE          0x900
#define CAN_ERROR_MASK          0x904
#define CAN_RXERRORCNT          0x910  /* rk3506.h CAN_REG */
#define CAN_TXERRORCNT          0x914  /* rk3506.h CAN_REG */

/* MODE bits */

#define MODE_WORK               (1u << 0)
#define MODE_LBACK              (1u << 4)

/* CMD bits */

#define CMD_TX0_REQ             (1u << 0)

/* INT / INT_MASK bits.  INT is w1c (set 1 to clear); in INT_MASK the
 * polarity is "1 = masked" (kernel: stop writes 0xffffffff, start writes
 * INT_ENABLE = BIT(0), masking only the unused per-frame RX_FINISH).
 */

#define INT_RX_FINISH           (1u << 0)
#define INT_TX_FINISH           (1u << 1)
#define INT_ERR_WARNING         (1u << 2)
#define INT_OVERLOAD            (1u << 3)  /* CMSIS OVERLOAD / kernel RX_BUF_OV */
#define INT_PASSIVE_ERR         (1u << 4)
#define INT_TX_LOSTARB          (1u << 5)
#define INT_BUS_ERR             (1u << 6)
#define INT_RXSTR_FULL          (1u << 7)
#define INT_RXSTR_OV            (1u << 8)
#define INT_BUSOFF              (1u << 9)  /* bus-off entered */
#define INT_BUSOFF_RECOVERY     (1u << 10) /* bus-off recovery state */
#define INT_AUTO_RETX_FAIL      (1u << 12) /* auto retransmission gave up */
#define INT_RXSTR_TIMEOUT       (1u << 15)
#define INT_ISM_WTM             (1u << 17)
#define INT_BUSOFF_RCY          (1u << 19) /* recovery sequence complete */

/* Enabled RX storage interrupts: watermark + storage timeout + FIFO full */

#define CAN_RX_IRQS             (INT_ISM_WTM | INT_RXSTR_TIMEOUT | \
                                 INT_RXSTR_FULL)

/* Error/status interrupts reported to the stack (CAN_ERR frames) and
 * used for bus-off recovery.
 */

#define CAN_ERR_IRQS            (INT_ERR_WARNING | INT_OVERLOAD | \
                                 INT_PASSIVE_ERR | INT_TX_LOSTARB | \
                                 INT_BUS_ERR | INT_RXSTR_OV | \
                                 INT_BUSOFF | INT_BUSOFF_RECOVERY | \
                                 INT_AUTO_RETX_FAIL | INT_BUSOFF_RCY)

/* Interrupts we service: TX completion, RX storage events, error /
 * bus-off events.  Everything else is explicitly masked; INT_MASK is a
 * full 32-bit write so we never depend on reset value of the high
 * halfword (kernel start leaves only RX_FINISH masked).
 */

#define CAN_INT_ENABLED         (INT_TX_FINISH | CAN_RX_IRQS | CAN_ERR_IRQS)
#define CAN_INT_MASK_VALUE      (0xffffffffu & ~CAN_INT_ENABLED)

/* STATE register status bits (kernel ERR_WARNING_STATE/BUS_OFF_STATE),
 * kept for reference alongside the CAN_STATE offset.
 */

#define STATE_ERR_WARNING       (1u << 3)
#define STATE_BUS_OFF           (1u << 4)

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

/* Bus-off recovery: fast counters at ifup (kernel), slow counters are
 * switched in on the first recovery interrupt (HAL/kernel
 * HAL_CANFD_BusOffRcy / rk3576_canfd_err).
 */

#define BUSOFF_RCY_EN           (1u << 8)
#define BUSOFF_RCY_SCLR         (1u << 9)
#define BUSOFF_RCY_CNT_FAST     4
#define BUSOFF_RCY_CNT_SLOW     5
#define BUSOFF_RCY_TIME_FAST    0x3d0900    /* 40 ms */
#define BUSOFF_RCY_TIME_SLOW    0x1312d00   /* 200 ms */

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

/* Bit timing table for CAN_CLK_HZ = 200 MHz, verbatim from
 * hal_canfd.c HAL_CANFD_SetNBps() (brq / tseg1 / tseg2 register
 * values): 1M/800k/500k/250k/200k/125k/100k/50k.
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
  {  800000,  4, 18, 4 },
  {  500000,  4, 33, 4 },
  {  250000,  4, 68, 9 },
  {  200000,  9, 43, 4 },
  {  125000,  9, 68, 9 },
  {  100000, 24, 33, 4 },
  {   50000, 24, 68, 9 },
};

/* RM_IO routing: one select register per physical pad
 * (rk3506.h struct RM_IO_REG, rk3506-pinctrl-rmio.dtsi) and one
 * function per pad; data = 0x7f0000 | (function_id - 15), then the
 * GPIO bank IOMUX nibble is set to 7 (RM_IO matrix source), see
 * u-boot/drivers/pinctrl/rockchip/pinctrl-rk3506.c rockchip_set_rmio().
 *
 * Function IDs (rk3506-pinctrl-rmio.dtsi):
 *   can1 tx/rx = 41/42, can0 tx/rx = 43/44.
 *
 * The two physical pads per controller are a board choice, taken from
 * CONFIG_RK3506_CANx_TX_PAD/RX_PAD (any bank0 pin 0..23 supported by
 * the RM_IO matrix; the SDK ships no can0/can1 pinctrl for the EVM, so
 * the default 255 = CAN_PAD_UNUSED keeps normal mode off the physical
 * pins instead of corrupting a pad with the old "two functions into
 * one register" sequence).  Loopback mode needs no pad at all and
 * keeps working on the bench.
 *
 * Routing is done through the unified rk3506_pinctrl_set_mux() (the
 * RM_IO matrix + IOMUX nibble); no local RM_IO/IOMUX register pokes.
 */

#define CAN0_TX_FN              43u
#define CAN0_RX_FN              44u

#define CAN1_TX_FN              41u
#define CAN1_RX_FN              42u

#define CAN_PAD_UNUSED          0xffu

/* The pad Kconfig options are hidden under !RK3506_CAN_LOOPBACK, so
 * in a loopback build the CONFIG_RK3506_CANx_*_PAD symbols do not
 * exist; fall back to "not routed" there.
 */

#ifndef CONFIG_RK3506_CAN0_TX_PAD
#  define CONFIG_RK3506_CAN0_TX_PAD  CAN_PAD_UNUSED
#endif
#ifndef CONFIG_RK3506_CAN0_RX_PAD
#  define CONFIG_RK3506_CAN0_RX_PAD  CAN_PAD_UNUSED
#endif
#ifndef CONFIG_RK3506_CAN1_TX_PAD
#  define CONFIG_RK3506_CAN1_TX_PAD  CAN_PAD_UNUSED
#endif
#ifndef CONFIG_RK3506_CAN1_RX_PAD
#  define CONFIG_RK3506_CAN1_RX_PAD  CAN_PAD_UNUSED
#endif

/* Deferred work flags (ISR only sets these, receive() work context
 * consumes them: no allocation / sleep in the ISR).
 */

#define CAN_EVT_ERRFRAME        (1u << 0)
#define CAN_EVT_TXABORT         (1u << 1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_can_s
{
  struct netdev_lowerhalf_s dev;        /* Must be first */
  uint32_t base;                        /* Controller register base */
  uint32_t bitrate;                     /* Configured bitrate */
  int irq;

  /* Physical RM_IO pads (CAN_PAD_UNUSED = not routed, see TODO above) */

  uint8_t tx_pad;
  uint8_t rx_pad;
  uint8_t tx_fn;                        /* RM_IO routing function, TX */
  uint8_t rx_fn;                        /* RM_IO routing function, RX */

  volatile bool rx_pending;             /* ISR saw RX/error work */
  volatile uint32_t isr_events;         /* CAN_EVT_* deferred to work */
  volatile uint32_t isr_errsrc;         /* Raw INT bits for CAN_ERR frame */

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

  /* SRST_CAN0 = SOFTRST_CON(13) bit 5, SRST_CAN1 = bit 7.  Pulse it
   * through the shared CRU soft-reset primitive (which uses the
   * correct RK3506_CRU_ADDR base) -- the previous code poked
   * 0xff480000 + ..., which is the SDMMC base, so the CAN reset never
   * actually fired.
   */

  rk3506_cru_reset(13, bit, true);
  up_udelay(2);
  rk3506_cru_reset(13, bit, false);

  putreg32(0, priv->base + CAN_MODE);
}

/****************************************************************************
 * Name: can_nbtp
 *
 * Description:
 *   Build the nominal bit timing register value for priv->bitrate.
 *   Returns -EINVAL for a bitrate the HAL has no table entry for - the
 *   controller must never silently run at a wrong speed (old code fell
 *   back to 500 kbit/s).
 *
 ****************************************************************************/

static int can_nbtp(FAR struct rk3506_can_s *priv, FAR uint32_t *nbtp)
{
  FAR const struct rk3506_can_bittime_s *bt = NULL;
  unsigned int i;

  for (i = 0; i < nitems(g_can_bittime); i++)
    {
      if (g_can_bittime[i].bitrate == priv->bitrate)
        {
          bt = &g_can_bittime[i];
          break;
        }
    }

  if (bt == NULL)
    {
      return -EINVAL;
    }

  *nbtp = (0u << NBTP_SJW_SHIFT) |
          (bt->brq << NBTP_BRQ_SHIFT) |
          (bt->tseg2 << NBTP_TSEG2_SHIFT) |
          (bt->tseg1 << NBTP_TSEG1_SHIFT);
  return OK;
}

/****************************************************************************
 * Name: can_rmio_route
 *
 * Description:
 *   Route one RM_IO function onto one physical pad: one RM_IO select
 *   register write (0x7f0000 WE + function) plus that pad's GPIO0
 *   IOMUX nibble write (mux 7).  One pad, one register, one function -
 *   the previous implementation wrote both CAN functions into the
 *   same pad's select register, so the second write overrode the first
 *   and TX could never leave the chip.
 *
 *   IOC register/nibble are computed from the pad index exactly like
 *   u-boot pinctrl-rk3506.c: reg = base + 8*(pad/8) + 4*((pad%8) >= 4),
 *   nibble = (pad % 4) * 4.
 *
 ****************************************************************************/

static void can_rmio_route(uint8_t pad, uint8_t func)
{
  /* The RM_IO matrix only reaches bank0 pins 0..23 (RM_IO0..23 =
   * GPIO0_A0..C7); a pad value outside that range is a misconfigured
   * CONFIG_RK3506_CANx_*_PAD and must not touch any register.
   */

  if (pad > 23u)
    {
      canerr("ERROR: CAN RM_IO pad %u out of range (0..23), not routed\n",
             pad);
      return;
    }

  /* bank0 pad (GPIO0 pin 0..23) routed through the RM_IO matrix:
   * rk3506_pinctrl_set_mux() sees func > 15 (CAN function ids 41-44),
   * programs the RM_IO channel with (func - 15) and then forces the
   * local IOMUX nibble to the matrix path (7).
   */

  rk3506_pinctrl_set_mux(0, pad, func);
}

/****************************************************************************
 * Name: can_pinmux
 *
 * Description:
 *   Route the controller TX/RX pads.  Both pads default to
 *   CAN_PAD_UNUSED (CONFIG_RK3506_CANx_TX_PAD/RX_PAD = 255), so this
 *   does nothing until a board defconfig assigns them.
 *
 ****************************************************************************/

static void can_pinmux(FAR struct rk3506_can_s *priv)
{
  if (priv->tx_pad != CAN_PAD_UNUSED)
    {
      can_rmio_route(priv->tx_pad, priv->tx_fn);
    }

  if (priv->rx_pad != CAN_PAD_UNUSED)
    {
      can_rmio_route(priv->rx_pad, priv->rx_fn);
    }
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
  uint32_t nbtp;
  int ret;

  if (priv->up)
    {
      return OK;
    }

  /* Validate the bitrate before touching clocks or pads: an unknown
   * rate must fail ifup explicitly instead of silently running at the
   * fallback timing (can_nbtp, hal_canfd.c table).
   */

  ret = can_nbtp(priv, &nbtp);
  if (ret < 0)
    {
      _err("can%u: unsupported bitrate %" PRIu32 "\n",
           priv->base == CAN0_BASE ? 0u : 1u, priv->bitrate);
      return ret;
    }

  /* Pads: hardware loopback is fully internal, never touch the pad
   * mux in the bench build.  In normal mode the pads stay unrouted
   * until a board fills them in (TODO(boards) in can_pinmux()).
   */

can_pinmux(priv);

  /* Clocks */

  rk3506_can_clock_init(priv->base == CAN0_BASE ? 0 : 1);

  /* Reset mode + register configuration */

  can_set_reset_mode(priv);

  /* Mask everything except TX completion, the RX storage event set and
   * the error/bus-off events (32-bit explicit write; kernel start uses
   * INT_ENABLE = BIT(0), masking only the unused per-frame RX_FINISH).
   */

  putreg32(CAN_INT_MASK_VALUE, priv->base + CAN_INT_MASK);

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

  putreg32(nbtp, priv->base + CAN_NBTP);
  putreg32(val | MODE_WORK, priv->base + CAN_MODE);

  priv->rx_pending  = false;
  priv->isr_events  = 0;
  priv->isr_errsrc  = 0;
  priv->up          = true;

  /* Interrupts */

  ret = irq_attach(priv->irq, can_interrupt, priv);
  if (ret < 0)
    {
      _err("can%u: irq_attach(%d) failed: %d\n",
           priv->base == CAN0_BASE ? 0u : 1u, priv->irq, ret);

      /* Return the controller to reset mode so a later ifup retry
       * starts from a stopped state.
       */

      can_set_reset_mode(priv);
      priv->up = false;
      return ret;
    }

  up_enable_irq(priv->irq);

  netdev_lower_carrier_on(dev);

  caninfo("can%u up: base=0x%08" PRIx32 " bitrate=%" PRIu32
          " NBTP=0x%08" PRIx32 " loopback=%d\n",
          priv->base == CAN0_BASE ? 0u : 1u,
          priv->base, priv->bitrate, nbtp,
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

  /* Release a pending TX pkt.  netpkt_free() restores the TX quota
   * itself (netdev_upperhalf.c netpkt_free), do not add it twice.
   */

  if (priv->tx_pkt != NULL)
    {
      netpkt_free(dev, priv->tx_pkt, NETPKT_TX);
      priv->tx_pkt = NULL;
    }

  priv->tx_confirm  = false;
  priv->rx_pending  = false;
  priv->isr_events  = 0;
  priv->isr_errsrc  = 0;
  priv->up          = false;
  return OK;
}

/****************************************************************************
 * Name: can_transmit
 *
 * Description:
 *   Write one classic CAN frame into the TX slot and request
 *   transmission (kernel rk3576_canfd_start_xmit, 1 in-flight frame).
 *
 *   Every validation happens BEFORE a hardware register is written or
 *   the upper-half TX packet/quota is taken over.  On a validation
 *   failure we return a negated errno without touching priv->tx_pkt;
 *   the upper half recycles the pkt and its TX quota
 *   (netdev_upperhalf.c netdev_upper_txpoll error path).
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

  /* Single in-flight slot: if the previous frame has not completed,
   * refuse before consuming anything.  The upper half stops the send
   * and retries after txdone (its quota gating normally prevents
   * this).
   */

  if (priv->tx_pkt != NULL)
    {
      return -EBUSY;
    }

  if (frame == NULL)
    {
      return -EINVAL;
    }

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

  /* Classic CAN only: clamp DLC to 8 (a legacy CAN socket never sends
   * DLC > 8; this is normalization, never a dropped frame).
   */

  len = frame->can_dlc;
  if (len > 8)
    {
      len = 8;
    }

  /* DLC is the plain length for classic CAN (kernel: can_fd_len2dlc
   * for FD frames; for classic frames len2dlc identity <= 8).
   */

  fic |= (len & TXFIC_DLC_MASK);

  /* Validation complete: from here on we own the pkt/quota until the
   * TX completion (or abort) in can_receive().
   */

  priv->tx_pkt = pkt;

  putreg32(id, priv->base + CAN_TXID);
  putreg32(fic, priv->base + CAN_TXFIC);

  for (i = 0; i < len; i += 4)
    {
      uint32_t word = 0;
      memcpy(&word, &frame->data[i], len - i < 4 ? len - i : 4);
      putreg32(word, priv->base + CAN_TXDATA + i);
    }

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
 *   The FIFO is pop-only: the 18 words must always be drained to
 *   advance it.  Unsupported (FD) frames are dropped after the drain
 *   but BEFORE allocating any upper-half RX packet, so a drop never
 *   consumes upper quota and never blocks the TX completion path.
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
      /* FD frames are dropped: stack runs classic CAN only.  No upper
       * quota is held, so can_receive() simply continues with the TX
       * completion / next frame.
       */

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
      /* Upper RX quota exhausted; the FIFO slot is already consumed.
       * The frame is dropped (same contract as ctucanfd_sock_recv);
       * TX completion handling in can_receive() is unaffected.
       */

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
 * Name: can_txfree
 *
 * Description:
 *   Release the in-flight TX pkt without any echo (TX completed in
 *   hardware loopback mode, where the looped RX frame is the echo, or
 *   TXCONFIRM is not configured) and restore TX quota.  netpkt_free()
 *   restores the quota itself.
 *
 ****************************************************************************/

static void can_txfree(FAR struct rk3506_can_s *priv)
{
  FAR struct netdev_lowerhalf_s *dev = &priv->dev;

  netpkt_free(dev, priv->tx_pkt, NETPKT_TX);
  priv->tx_pkt     = NULL;
  priv->tx_confirm = false;
}

/****************************************************************************
 * Name: can_txconfirm
 *
 * Description:
 *   Return the in-flight TX pkt with the CAN_TCF flag (the local
 *   SocketCAN echo/transmit confirmation), restoring TX quota while
 *   consuming one RX quota slot (ctucanfd_sock_txconfirm pattern).
 *
 ****************************************************************************/

#if defined(CONFIG_NET_CAN_TXCONFIRM) && !defined(CONFIG_RK3506_CAN_LOOPBACK)
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
 * Name: can_txabort
 *
 * Description:
 *   Discard the in-flight TX pkt after a terminal failure reported by
 *   the controller (bus-off recovery / auto-retx-fail).  The frame was
 *   not delivered, so there is no TCF echo - mirror the kernel
 *   can_free_echo_skb() in rk3576_canfd_err(): free as TX and give the
 *   quota back, then wake the TX path.  Called in receive() work
 *   context (never in the ISR).
 *
 ****************************************************************************/

static void can_txabort(FAR struct rk3506_can_s *priv)
{
  FAR struct netdev_lowerhalf_s *dev = &priv->dev;

  if (priv->tx_pkt != NULL)
    {
      /* netpkt_free() restores the TX quota itself */

      netpkt_free(dev, priv->tx_pkt, NETPKT_TX);
      priv->tx_pkt = NULL;
    }

  priv->tx_confirm = false;
  netdev_lower_txdone(dev);
}

#ifdef CONFIG_NET_CAN_ERRORS
/****************************************************************************
 * Name: can_error_frame
 *
 * Description:
 *   Build one SocketCAN error message frame (CAN_ERR_FLAG, DLC 8) from
 *   a snapshot of raw INT source bits, following the classification of
 *   kernel rk3576_canfd_err(): bus-off, error warning/passive, bus
 *   error, arbitration lost, RX overflow, restarted after recovery.
 *   Called in receive() work context; returns NULL if no class applies
 *   or no RX pkt can be allocated.
 *
 ****************************************************************************/

static FAR netpkt_t *can_error_frame(FAR struct netdev_lowerhalf_s *dev,
                                     uint32_t src)
{
  FAR struct rk3506_can_s *priv = (FAR struct rk3506_can_s *)dev;
  FAR netpkt_t *pkt;
  FAR struct can_frame *frame;
  uint32_t errbits = 0;
  uint8_t txerr;
  uint8_t rxerr;

  if (src & (INT_BUSOFF | INT_BUSOFF_RECOVERY))
    {
      errbits |= CAN_ERR_BUSOFF;
    }

  if (src & INT_BUSOFF_RCY)
    {
      /* Recovery sequence completed: controller was restarted */

      errbits |= CAN_ERR_RESTARTED;
    }

  if (src & INT_PASSIVE_ERR)
    {
      errbits |= CAN_ERR_CRTL;
    }

  if (src & (INT_ERR_WARNING))
    {
      errbits |= CAN_ERR_CRTL;
    }

  if (src & INT_BUS_ERR)
    {
      errbits |= CAN_ERR_BUSERROR;

      /* TODO(real-bus): decode CAN_ERROR_CODE (0x900) into
       * CAN_ERR_PROT class/location per rk3576_canfd.c error phase
       * fields; needs a real bus to validate.
       */
    }

  if (src & INT_TX_LOSTARB)
    {
      errbits |= CAN_ERR_LOSTARB;
    }

  if (src & (INT_OVERLOAD | INT_RXSTR_OV))
    {
      errbits |= CAN_ERR_CRTL;
    }

  if (src & INT_AUTO_RETX_FAIL)
    {
      /* Retransmission stopped after the 300-try limit: on a real
       * two-wire bus this is almost always a missing ACK.
       * TODO(real-bus): validate the exact trigger condition.
       */

      errbits |= CAN_ERR_ACK;
    }

  if (errbits == 0)
    {
      return NULL;
    }

  pkt = netpkt_alloc(dev, NETPKT_RX);
  if (pkt == NULL)
    {
      canerr("error frame alloc failed\n");
      return NULL;
    }

  netpkt_setdatalen(dev, pkt, sizeof(struct can_frame));
  frame = (FAR struct can_frame *)netpkt_getdata(dev, pkt);
  memset(frame, 0, sizeof(*frame));

  txerr = (uint8_t)(getreg32(priv->base + CAN_TXERRORCNT) & 0xffu);
  rxerr = (uint8_t)(getreg32(priv->base + CAN_RXERRORCNT) & 0xffu);

  if ((errbits & CAN_ERR_CRTL) != 0)
    {
      if (src & INT_PASSIVE_ERR)
        {
          frame->data[1] |= (txerr > rxerr) ?
                            CAN_ERR_CRTL_TX_PASSIVE :
                            CAN_ERR_CRTL_RX_PASSIVE;
        }
      else
        {
          frame->data[1] |= (txerr > rxerr) ?
                            CAN_ERR_CRTL_TX_WARNING :
                            CAN_ERR_CRTL_RX_WARNING;
        }

      if (src & (INT_OVERLOAD | INT_RXSTR_OV))
        {
          frame->data[1] |= CAN_ERR_CRTL_RX_OVERFLOW;
        }
    }

  if ((errbits & CAN_ERR_LOSTARB) != 0)
    {
      frame->data[0] = CAN_ERR_LOSTARB_UNSPEC;
    }

  frame->data[6] = txerr;
  frame->data[7] = rxerr;

  frame->can_id  = CAN_ERR_FLAG | errbits;
  frame->can_dlc = CAN_ERR_DLC;
  return pkt;
}
#endif /* CONFIG_NET_CAN_ERRORS */

/****************************************************************************
 * Name: can_receive
 *
 * Description:
 *   Drain pending work in a fixed order matching the NuttX lower-half
 *   contract (ctucanfd_sock_recv / vcan_receive):
 *     1. terminal TX abort    - free the in-flight frame, return quota
 *     2. TX completion       - CAN_TCF echo (normal) / silent free
 *                              (hardware loopback: the RX FIFO frame
 *                              is the only echo)
 *     3. error frames        - CAN_ERR message frames
 *     4. RX stream FIFO      - received frames
 *
 ****************************************************************************/

static FAR netpkt_t *can_receive(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct rk3506_can_s *priv = (FAR struct rk3506_can_s *)dev;
  FAR netpkt_t *pkt;
  irqstate_t flags;
  uint32_t events;
  uint32_t words;

  /* 1. Terminal TX failure (bus-off recovery / retx exhausted) */

  flags = enter_critical_section();
  events = priv->isr_events;
  if ((events & CAN_EVT_TXABORT) != 0)
    {
      priv->isr_events &= ~CAN_EVT_TXABORT;
    }

  leave_critical_section(flags);

  if ((events & CAN_EVT_TXABORT) != 0)
    {
      can_txabort(priv);
    }

  /* 2. TX completion.  In hardware loopback the controller already
   * hands the frame back through the RX FIFO (step 4) - emitting the
   * CAN_TCF echo as well would deliver every sent frame twice, so free
   * the in-flight pkt instead (vcan.c-style single echo).
   */

  if (priv->tx_confirm && priv->tx_pkt != NULL)
    {
#if defined(CONFIG_NET_CAN_TXCONFIRM) && !defined(CONFIG_RK3506_CAN_LOOPBACK)
      pkt = can_txconfirm(priv);
      if (pkt != NULL)
        {
          return pkt;
        }
#else
      can_txfree(priv);
#endif
    }

  /* 3. Error message frame (one per receive() call) */

#ifdef CONFIG_NET_CAN_ERRORS
  {
    uint32_t errsrc;

    flags = enter_critical_section();
    if ((priv->isr_events & CAN_EVT_ERRFRAME) != 0)
      {
        errsrc = priv->isr_errsrc;
        priv->isr_errsrc   = 0;
        priv->isr_events &= ~CAN_EVT_ERRFRAME;
      }
    else
      {
        errsrc = 0;
      }

    leave_critical_section(flags);

    if (errsrc != 0)
      {
        pkt = can_error_frame(dev, errsrc);
        if (pkt != NULL)
          {
            return pkt;
          }
      }
  }
#endif

  /* 4. Frames waiting in the internal SRAM? */

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
 *   CMD=0 + wake), RX stream events flag pending frames, error events
 *   are latched for the receive() work thread and bus-off recovery is
 *   reconfigured to the slow counters (kernel rk3576_canfd_err /
 *   HAL_CANFD_BusOffRcy).  Only register accesses happen here - never
 *   allocation or sleeping.
 *
 ****************************************************************************/

static int can_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3506_can_s *priv = (FAR struct rk3506_can_s *)arg;
  uint32_t isr;
  uint32_t err;
  bool do_rx = false;
  bool do_tx = false;

  isr = getreg32(priv->base + CAN_INT);

  if (isr & INT_TX_FINISH)
    {
      putreg32(0, priv->base + CAN_CMD);
      priv->tx_confirm = true;
      do_tx = true;
    }

  if (isr & CAN_RX_IRQS)
    {
      priv->rx_pending = true;
      do_rx = true;
    }

  err = isr & CAN_ERR_IRQS;
  if (err != 0)
    {
      if (err & INT_BUSOFF_RCY)
        {
          /* Recovery sequence complete: switch to slow recovery
           * timing (kernel rk3576_canfd_err, HAL_CANFD_BusOffRcy).
           * Pure register accesses - safe in the ISR.
           */

          putreg32(0xffffffffu, priv->base + CAN_INT_MASK);
          putreg32(BUSOFF_RCY_SCLR, priv->base + CAN_BUSOFF_RCY_CFG);
          putreg32(BUSOFF_RCY_TIME_SLOW, priv->base + CAN_BUSOFF_RCY_THR);
          putreg32(BUSOFF_RCY_EN | BUSOFF_RCY_CNT_SLOW,
                   priv->base + CAN_BUSOFF_RCY_CFG);
          putreg32(CAN_INT_MASK_VALUE, priv->base + CAN_INT_MASK);
        }

      /* Latch for the work thread: error frame (if the stack wants
       * them) and in-flight frame teardown (can_free_echo_skb
       * equivalent in can_receive(), needed regardless).
       */

#ifdef CONFIG_NET_CAN_ERRORS
      priv->isr_errsrc |= err;
      priv->isr_events |= CAN_EVT_ERRFRAME;
#endif

      if (err & (INT_BUSOFF_RCY | INT_AUTO_RETX_FAIL))
        {
          priv->isr_events |= CAN_EVT_TXABORT;
        }

      priv->rx_pending = true;
      do_rx = true;
    }

  if (isr != 0)
    {
      /* Clear the handled interrupts (w1c) */

      putreg32(isr, priv->base + CAN_INT);

      if (do_rx)
        {
          netdev_lower_rxready(&priv->dev);
        }

      if (do_tx)
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
 *   No chip-specific ioctls yet.  The configured bitrate is validated
 *   in can_ifup(); runtime bitrate changes are not wired up.
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

      /* IFF_LOOPBACK marks a loopback link only: set it just for the
       * hardware loopback bench build (like vcan.c).  In normal mode
       * the local echo is the CAN_TCF confirmation from can_receive(),
       * exactly as on ctucanfd.
       */

#ifdef CONFIG_RK3506_CAN_LOOPBACK
      IFF_SET_LOOPBACK(priv->dev.netdev.d_flags);
#endif

      snprintf(priv->dev.netdev.d_ifname, IFNAMSIZ, "can%d", i);

      if (i == 0)
        {
          priv->base    = CAN0_BASE;
          priv->irq     = CAN_IRQ0;
          priv->bitrate = CONFIG_RK3506_CAN0_BITRATE;
          priv->tx_fn   = CAN0_TX_FN;
          priv->rx_fn   = CAN0_RX_FN;
          priv->tx_pad  = (uint8_t)CONFIG_RK3506_CAN0_TX_PAD;
          priv->rx_pad  = (uint8_t)CONFIG_RK3506_CAN0_RX_PAD;
        }
      else
        {
          priv->base    = CAN1_BASE;
          priv->irq     = CAN_IRQ1;
          priv->bitrate = CONFIG_RK3506_CAN1_BITRATE;
          priv->tx_fn   = CAN1_TX_FN;
          priv->rx_fn   = CAN1_RX_FN;
          priv->tx_pad  = (uint8_t)CONFIG_RK3506_CAN1_TX_PAD;
          priv->rx_pad  = (uint8_t)CONFIG_RK3506_CAN1_RX_PAD;
        }

      /* TX/RX pads come from CONFIG_RK3506_CANx_TX_PAD/RX_PAD (board
       * choice, default 255 = CAN_PAD_UNUSED = stay off the physical
       * pins until a board defconfig assigns them).  can_pinmux()
       * skips any pad still marked CAN_PAD_UNUSED.
       */

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
