/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_gmac0.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506 GMAC0 (Synopsys DWMAC 4.20a) NuttX Ethernet driver.
 *
 * Architecture:
 *   - ALL hardware sequences come from the Linux SDK GMAC HAL ported
 *     verbatim in rk3506_gmac_hal.c (hal_gmac.c + hal_gmac_rk3506.c):
 *     HAL_GMAC_Init/Start/Stop/AdjustLink, the descriptor management,
 *     the generic PHY state machine and the DMA IRQ dispatcher.
 *   - This file is the NuttX netdev integration shell.  Its structure
 *     follows the in-tree drivers/net/dm90x0.c pattern:
 *     d_buf single packet buffer, net_lock()ed work-queue interrupt
 *     processing, devif_poll() for TX, ipv4/ipv6/arp_input dispatch,
 *     a link-status watchdog and netdev_register(NET_LL_ETHERNET).
 *   - Board/SoC glue kept from the bring-up phase (all verified on
 *     hardware): CRU clock tree (rk3506_cru.c rk3506_gmac0_clock_init),
 *     RMII IOMUX (rk3506_iomux.c) and the GPIO4_A2 PHY reset.
 *
 * Hardware facts (HD-RK3506-EVM board docs + RK3506 SDK):
 *   - GMAC0 @ 0xFF4C8000, macirq = GIC_SPI 66 = RK3506_IRQ_GMAC0_0
 *     (rk3506.dtsi gmac0: interrupts = <GIC_SPI 66 IRQ_TYPE_LEVEL_HIGH>).
 *   - PHY: Motorcomm YT8512B RMII @ MDIO addr 0 (ID 0x00000128, both
 *     verified on hardware and stated by the board vendor doc
 *     HD-RK3506-EVM [通用]硬件设计/硬件调试与故障分析.md).
 *   - RMII reference clock: clock_in_out = "input" (HD-RK3506-EVM
 *     board doc SDK开发与外设设备树配置/GMAC.md): the PHY's
 *     crystal-locked 50 MHz drives GPIO2_B2 into the MAC, so
 *     GRF_SOC_CON8 bit5 must be 1
 *     (RK3506_MAC_CLK_SELET_IO = CONFIG_RK3506_GMAC0_EXTCLK=y).  The
 *     Rockchip in-house EVM uses the opposite direction ("output");
 *     copying its direction leaves the MAC fighting the PHY over the
 *     clock line: MDIO stays alive (independent MDC) but autoneg never
 *     completes, i.e. "MDIO probe: PHY at addr 0" + "no link yet".
 *   - MDIO CSR: pclk = hclk_lsperi = gpll_div/1 = ~297 MHz falls in the
 *     HAL's [250,300) -> CSR=5 bucket (empirically the CSR that works).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_RK3506_GMAC0

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>

#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/signal.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>
#include <nuttx/spinlock.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/netdev.h>
#include <nuttx/cache.h>

#include "hardware/rk3506_memorymap.h"

#ifndef putreg32
#  define putreg32(v, a) (*(FAR volatile uint32_t *)(a) = (v))
#endif
#ifndef getreg32
#  define getreg32(a)    (*(FAR volatile uint32_t *)(a))
#endif
#include "rk3506_cru.h"
#include "rk3506_iomux.h"
#include "rk3506_gmac_hal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The low priority work queue is preferred (see dm90x0.c): the network
 * must never run on the high priority work queue.
 */

#define ETHWORK LPWORK

/* Ring sizes (power of two; GMAC_GET_ENTRY uses & (size-1)). */

#define ETH_TXBUFNB 4
#define ETH_RXBUFNB 4

/* TX timeout and link poll watchdogs */

#define GMAC0_TXTIMEOUT  (5 * CLK_TCK)
#define GMAC0_LINKPOLL   (2 * CLK_TCK)

/* Bounded wait for the first link-up inside ifup: covers typical RMII
 * autoneg (2-4 s from the PHY reset).  Costs nothing when the link is
 * already up; on timeout ifup still succeeds and the link poll takes
 * over.
 */

#define GMAC0_LINKWAIT_MS 5000

/* Packet buffer for the network stack (dm90x0.c pattern) */

#define PKTBUF_SIZE (MAX_NETDEV_PKTSIZE + CONFIG_NET_GUARDSIZE)

/* PHY hardware reset: GPIO4_A2, active low (HD-RK3506-EVM GMAC.md:
 * snps,reset-gpio = <&gpio4 RK_PA2 GPIO_ACTIVE_LOW>,
 * reset-delays-us = <0 20000 100000>).  GPIO4 v2 controller base
 * 0xFF1E0000 (rk3506.h GPIO4_BASE); A2 = pin 2 lives in the _L register
 * pair (pins 0..15): SWPORT_DR_L = +0x00, SWPORT_DDR_L = +0x08, bit 2.
 * NOTE: GPIO0_C2 was the Rockchip in-house EVM value and is NOT the
 * pin wired to the PHY reset on this board.
 */

#define RK3506_GPIO4_BASE              0xFF1E0000u
#define GPIO_SWPORT_DR_L               0x00u
#define GPIO_SWPORT_DDR_L              0x08u
#define PHY_RESET_PIN_BIT              (1u << 2)

/* MDIO clock source frequency fed to HAL_GMAC_Init() for the CSR
 * selection.  hclk_lsperi_root: CLKSEL_CON(29) mux[6:5]=0 (clk_gpll_div)
 * div[4:0]=1; clk_gpll_div = gpll/4 ~= 297 MHz -> CSR bucket [250,300)
 * = GMAC_CSR_250_300M = 5, the empirically-verified MDC divider.
 */

#define RK3506_GMAC0_MDIO_FREQ  297000000

/* Default MAC address (locally administered; overridable via ioctl) */

#define GMAC0_DEFAULT_MAC  { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }

/* Ethernet frame header access for the RX dispatch (dm90x0.c) */

#define BUF ((FAR struct eth_hdr_s *)priv->dev.d_buf)

/* Clause-22 PHY ID registers (non-destructive early probe) */

#define MII_PHYSID1 0x02
#define MII_PHYSID2 0x03
#define MII_BMCR    0x00

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_gmac0_s
{
  struct net_driver_s dev;       /* Interface understood by the network */
  struct GMAC_HANDLE  gmac;      /* SDK HAL handle (verbatim port) */

  bool bifup;                    /* true: ifup done */
  struct wdog_s txtimeout;       /* TX timeout watchdog */
  struct wdog_s linktimer;       /* Link poll watchdog */

  struct work_s irqwork;         /* Interrupt deferred work */
  struct work_s pollwork;        /* Link poll / timeout deferred work */
  struct work_s txavailwork;     /* TX avail deferred work */

#ifdef CONFIG_NETDEV_PHY_IOCTL
  /* SIOCMIINOTIFY registrant: the netinit monitor (or any PHY-status
   * client) registers a sigevent and is signalled on link changes.
   */

  pid_t notify_pid;              /* PID to signal, 0 = none */
  struct sigevent notify_event;  /* As passed with SIOCMIINOTIFY */
#endif
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3506_gmac0_s g_gmac0;

/* Descriptor rings and buffers: static, 64-byte aligned.  The DMA
 * accesses these physically; the CPU side keeps coherence with
 * up_clean_dcache()/up_invalidate_dcache(): clean before Send / the
 * descriptor hand-back, invalidate before reading the frame buffer,
 * and (added on top of the SDK HAL, see rk3506_gmac_hal.c) invalidate
 * before every descriptor STATUS read - without the last one the CPU
 * never sees the DMA's OWN-bit writebacks.
 */

static struct GMAC_Desc g_tx_desc[ETH_TXBUFNB]
  __attribute__((aligned(64)));
static struct GMAC_Desc g_rx_desc[ETH_RXBUFNB]
  __attribute__((aligned(64)));

static uint8_t g_tx_buf[ETH_TXBUFNB * HAL_GMAC_MAX_PACKET_SIZE]
  __attribute__((aligned(64)));

/* RX buffers are RBSZ-sized (see HAL_GMAC_RXBUF_SIZE in the HAL header:
 * HAL_GMAC_Start programs RBSZ = the 8 KiB HW RX FIFO size, so the DMA
 * may write up to that per descriptor).
 */

static uint8_t g_rx_buf[ETH_RXBUFNB * HAL_GMAC_RXBUF_SIZE]
  __attribute__((aligned(64)));

/* Single packet buffer for the network stack */

static uint16_t g_pktbuf[(PKTBUF_SIZE + 1) / 2];

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  rk3506_gmac0_transmit(FAR struct rk3506_gmac0_s *priv);
static void rk3506_gmac0_txavail_work(FAR void *arg);
static int  rk3506_gmac0_txavail(FAR struct net_driver_s *dev);
static int  rk3506_gmac0_ifup(FAR struct net_driver_s *dev);
static int  rk3506_gmac0_ifdown(FAR struct net_driver_s *dev);
#ifdef CONFIG_NETDEV_PHY_IOCTL
static int  rk3506_gmac0_ioctl(FAR struct net_driver_s *dev, int cmd,
                               unsigned long arg);
#endif
static void rk3506_gmac0_txtimeout_expiry(wdparm_t arg);
static void rk3506_gmac0_linkpoll_expiry(wdparm_t arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_gmac0_yt8512b_init
 *
 * Description:
 *   Board PHY init hook for the Motorcomm YT8512B (MDIO ID 0x00000128,
 *   verified on hardware), ported from the Linux SDK PHY driver
 *   kernel-6.1/drivers/net/phy/motorcomm.c yt8512_config_init() /
 *   yt8512_clk_init() - the parts that matter for RMII operation
 *   (the LED cosmetics are skipped):
 *
 *     - ext reg 0x50 (YT8512_EXTREG_AFE_PLL): set bit6
 *       (YT8512_CONFIG_PLL_REFCLK_SEL_EN) so the PHY's PLL takes the
 *       external REFCLK input (board: clock_in_out = "output", the MAC
 *       drives 50 MHz into the PHY).
 *     - ext reg 0x4000 (YT8512_EXTREG_EXTEND_COMBO): set bit0
 *       (YT8512_CONTROL1_RMII_EN) to put the MII side into RMII mode.
 *     - ext reg 0x2027 (YT8512_EXTREG_SLEEP_CONTROL1): clear bit15
 *       (EN_SLEEP_SW) to disable auto-sleep.
 *     - BMCR soft reset to apply.
 *
 *   Without the first two writes the PHY's MII side does not speak
 *   RMII: the MAC then sees systematically corrupted frames (measured:
 *   every received frame counted as CRC error, tx frames never left).
 *   Extended registers are reached via the standard Motorcomm debug
 *   port: reg 0x1e = address, reg 0x1f = data (ytphy_read_ext /
 *   ytphy_write_ext in motorcomm.c).
 *
 ****************************************************************************/

#define YT8512_MII_REG_DEBUG_ADDR       0x1e
#define YT8512_MII_REG_DEBUG_DATA       0x1f

#define YT8512_EXTREG_AFE_PLL           0x0050
#define YT8512_EXTREG_EXTEND_COMBO      0x4000
#define YT8512_EXTREG_SLEEP_CONTROL1    0x2027

#define YT8512_CONFIG_PLL_REFCLK_SEL_EN 0x0040
#define YT8512_CONTROL1_RMII_EN         0x0001
#define YT8512_EN_SLEEP_SW_BIT          15

#define YT8512_BMCR_SOFTWARE_RESET      0x8000

static int rk3506_gmac0_yt8512b_ext_read(FAR struct GMAC_HANDLE *gmac,
                                         int addr, uint32_t extreg)
{
  if (HAL_GMAC_MDIOWrite(gmac, addr, YT8512_MII_REG_DEBUG_ADDR,
                         (uint32_t)extreg) < 0)
    {
      return -1;
    }

  return HAL_GMAC_MDIORead(gmac, addr, YT8512_MII_REG_DEBUG_DATA);
}

static int rk3506_gmac0_yt8512b_ext_write(FAR struct GMAC_HANDLE *gmac,
                                          int addr, uint32_t extreg,
                                          uint32_t val)
{
  if (HAL_GMAC_MDIOWrite(gmac, addr, YT8512_MII_REG_DEBUG_ADDR,
                         (uint32_t)extreg) < 0)
    {
      return -1;
    }

  return HAL_GMAC_MDIOWrite(gmac, addr, YT8512_MII_REG_DEBUG_DATA, val);
}

static int rk3506_gmac0_yt8512b_init(FAR struct GMAC_HANDLE *gmac)
{
  int addr = (int)gmac->phyStatus.addr;
  int val;
  int ret = HAL_OK;

  /* yt8512_clk_init(): the HD-RK3506-EVM is a clock_in_out = "input"
   * board (the PHY drives REF_CLK out of GPIO2_B2 into the MAC and
   * takes its PLL reference from its own crystal), which selects
   * CONFIG_RK3506_GMAC0_EXTCLK: like the Linux SDK motorcomm.c
   * yt8512_clk_init() for "input", the PLL-refclk / RMII vendor
   * writes are skipped and the PHY keeps its strap defaults.  The
   * writes below only run for "output" boards (Rockchip in-house
   * EVM dts, CONFIG_RK3506_GMAC0_EXTCLK unset).
   */

#ifndef CONFIG_RK3506_GMAC0_EXTCLK
  val = rk3506_gmac0_yt8512b_ext_read(gmac, addr, YT8512_EXTREG_AFE_PLL);
  if (val < 0)
    {
      return HAL_ERROR;
    }

  val |= YT8512_CONFIG_PLL_REFCLK_SEL_EN;
  if (rk3506_gmac0_yt8512b_ext_write(gmac, addr, YT8512_EXTREG_AFE_PLL,
                                     (uint32_t)val) < 0)
    {
      return HAL_ERROR;
    }

  val = rk3506_gmac0_yt8512b_ext_read(gmac, addr,
                                      YT8512_EXTREG_EXTEND_COMBO);
  if (val < 0)
    {
      return HAL_ERROR;
    }

  val |= YT8512_CONTROL1_RMII_EN;
  if (rk3506_gmac0_yt8512b_ext_write(gmac, addr, YT8512_EXTREG_EXTEND_COMBO,
                                     (uint32_t)val) < 0)
    {
      return HAL_ERROR;
    }
#endif

  /* yt8512_config_init(): disable auto sleep */

  val = rk3506_gmac0_yt8512b_ext_read(gmac, addr,
                                      YT8512_EXTREG_SLEEP_CONTROL1);
  if (val < 0)
    {
      return HAL_ERROR;
    }

  val &= ~(1u << YT8512_EN_SLEEP_SW_BIT);
  if (rk3506_gmac0_yt8512b_ext_write(gmac, addr,
                                     YT8512_EXTREG_SLEEP_CONTROL1,
                                     (uint32_t)val) < 0)
    {
      return HAL_ERROR;
    }

  /* Apply with a BMCR soft reset (motorcomm.c: val |= YT_SOFTWARE_RESET) */

  val = HAL_GMAC_MDIORead(gmac, addr, MII_BMCR);
  if (val < 0)
    {
      return HAL_ERROR;
    }

  if (HAL_GMAC_MDIOWrite(gmac, addr, MII_BMCR,
                         (uint32_t)val | YT8512_BMCR_SOFTWARE_RESET) < 0)
    {
      return HAL_ERROR;
    }

  up_udelay(100 * 1000u);   /* ~100ms, like HAL_DelayUs(100000) */

  return ret;
}

/****************************************************************************
 * Name: rk3506_gmac0_phy_reset
 *
 * Description:
 *   Hardware-reset the Ethernet PHY via GPIO4_A2 (active low), per
 *   the board vendor doc (HD-RK3506-EVM, SDK开发与外设设备树配置
 *   /GMAC.md):
 *     reset-gpios = <&gpio4 RK_PA2 GPIO_ACTIVE_LOW>;
 *     reset-delays-us = <0 20000 100000>;
 *   GPIO v2 controller HIWORD semantics: writing (bit<<16)|value sets
 *   the bit to value.  DDR bit = 1 means OUTPUT.  A2 = pin 2 -> the _L
 *   register pair (SWPORT_DR_L / SWPORT_DDR_L).
 *
 ****************************************************************************/

static void rk3506_gmac0_phy_reset(void)
{
  uint32_t dr = RK3506_GPIO4_BASE + GPIO_SWPORT_DR_L;
  uint32_t ddr = RK3506_GPIO4_BASE + GPIO_SWPORT_DDR_L;

  /* Set the pin direction to OUTPUT first. */

  putreg32((PHY_RESET_PIN_BIT << 16) | PHY_RESET_PIN_BIT, ddr);

  /* Drive low: PHY held in reset. */

  putreg32((PHY_RESET_PIN_BIT << 16) | 0u, dr);

  up_udelay(20 * 1000u);

  /* Release reset: drive high. */

  putreg32((PHY_RESET_PIN_BIT << 16) | PHY_RESET_PIN_BIT, dr);

  up_udelay(100 * 1000u);
}

/****************************************************************************
 * Name: rk3506_gmac0_receive
 *
 * Description:
 *   Drain completed RX descriptors and feed the frames to the network.
 *   dm90x0.c dm9x_receive() pattern with the SDK HAL Recv/CleanRX.
 *
 * Assumptions:
 *   Network locked (net_lock held by the caller).
 *
 ****************************************************************************/

static void rk3506_gmac0_receive(FAR struct rk3506_gmac0_s *priv)
{
  FAR struct net_driver_s *dev = &priv->dev;
  FAR uint8_t *rxbuf;
  int32_t len;

  do
    {
      rxbuf = HAL_GMAC_Recv(&priv->gmac, &len);
      if (rxbuf == NULL)
        {
          break;   /* No more completed RX descriptors */
        }

      /* The DMA wrote len + 4 (FCS) bytes; invalidate them. */

      up_invalidate_dcache((uintptr_t)rxbuf,
                           (uintptr_t)rxbuf + len + 4);

      /* Copy the frame into the network buffer (d_buf is a single
       * PKTBUF_SIZE buffer; frame length is bounded by the HAL's
       * HAL_GMAC_MAX_FRAME_SIZE check in HAL_GMAC_Recv).
       */

      memcpy(dev->d_buf, rxbuf, len);

      /* Hand the descriptor back and bump the RX tail pointer. */

      HAL_GMAC_CleanRX(&priv->gmac);

      dev->d_len = len;

#ifdef CONFIG_NET_PKT
      pkt_input(dev);
#endif

#ifdef CONFIG_NET_IPv4
      if (BUF->type == HTONS(ETHTYPE_IP))
        {
          NETDEV_RXIPV4(dev);
          ipv4_input(dev);

          if (dev->d_len > 0)
            {
              rk3506_gmac0_transmit(priv);
            }
        }
      else
#endif
#ifdef CONFIG_NET_IPv6
      if (BUF->type == HTONS(ETHTYPE_IP6))
        {
          NETDEV_RXIPV6(dev);
          ipv6_input(dev);

          if (dev->d_len > 0)
            {
              rk3506_gmac0_transmit(priv);
            }
        }
      else
#endif
#ifdef CONFIG_NET_ARP
      if (BUF->type == HTONS(ETHTYPE_ARP))
        {
          NETDEV_RXARP(dev);
          arp_input(dev);

          if (dev->d_len > 0)
            {
              rk3506_gmac0_transmit(priv);
            }
        }
      else
#endif
        {
          NETDEV_RXDROPPED(dev);
        }

      NETDEV_RXPACKETS(dev);
    }
  while (true);
}

/****************************************************************************
 * Name: rk3506_gmac0_transmit
 *
 * Description:
 *   Start hardware transmission: copy the network buffer into the next
 *   TX ring buffer and kick the DMA (SDK HAL_GMAC_Send verbatim).
 *
 * Assumptions:
 *   Network locked.
 *
 ****************************************************************************/

static int rk3506_gmac0_transmit(FAR struct rk3506_gmac0_s *priv)
{
  FAR struct net_driver_s *dev = &priv->dev;
  FAR uint8_t *txbuf;
  int ret;

  if (dev->d_len == 0)
    {
      return OK;
    }

  NETDEV_TXPACKETS(dev);

  txbuf = HAL_GMAC_GetTXBuffer(&priv->gmac);
  memcpy(txbuf, dev->d_buf, dev->d_len);

  /* SDK HAL doc: "Must clean the dcached memory before use
   * HAL_GMAC_Send()".
   */

  up_clean_dcache((uintptr_t)txbuf, (uintptr_t)txbuf + dev->d_len);

  ret = HAL_GMAC_Send(&priv->gmac, txbuf, dev->d_len);
  if (ret != HAL_OK)
    {
      /* Ring full: the TX-done IRQ will poll the network again */

      NETDEV_TXERRORS(dev);
      return -EBUSY;
    }

  /* Setup the TX timeout watchdog (dm90x0 pattern). */

  wd_start(&priv->txtimeout, GMAC0_TXTIMEOUT,
           rk3506_gmac0_txtimeout_expiry, (wdparm_t)priv);
  return OK;
}

/****************************************************************************
 * Name: rk3506_gmac0_txpoll
 *
 * Description:
 *   devif_poll() callback: transmit the queued packet if any.
 *
 ****************************************************************************/

static int rk3506_gmac0_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct rk3506_gmac0_s *priv =
    (FAR struct rk3506_gmac0_s *)dev->d_private;

  rk3506_gmac0_transmit(priv);

  /* Single TX packet buffer: one packet per poll pass. */

  return 1;
}

/****************************************************************************
 * Name: rk3506_gmac0_txdone
 *
 * Description:
 *   TX interrupt work: poll the network for the next TX packet.
 *
 ****************************************************************************/

static void rk3506_gmac0_txdone(FAR struct rk3506_gmac0_s *priv)
{
  NETDEV_TXDONE(&priv->dev);

  wd_cancel(&priv->txtimeout);

  devif_poll(&priv->dev, rk3506_gmac0_txpoll);
}

/****************************************************************************
 * Name: rk3506_gmac0_txtimeout_work / _expiry
 *
 * Description:
 *   TX watchdog: if a frame sat too long in the ring, re-poll.
 *
 ****************************************************************************/

static void rk3506_gmac0_txtimeout_work(FAR void *arg)
{
  FAR struct rk3506_gmac0_s *priv = (FAR struct rk3506_gmac0_s *)arg;

  net_lock();
  if (priv->bifup)
    {
      NETDEV_TXERRORS(&priv->dev);
      devif_poll(&priv->dev, rk3506_gmac0_txpoll);
    }
  net_unlock();
}

static void rk3506_gmac0_txtimeout_expiry(wdparm_t arg)
{
  FAR struct rk3506_gmac0_s *priv = (FAR struct rk3506_gmac0_s *)arg;

  work_queue(ETHWORK, &priv->pollwork, rk3506_gmac0_txtimeout_work,
             priv, 0);
}

/****************************************************************************
 * Name: rk3506_gmac0_linkpoll_work / _expiry
 *
 * Description:
 *   Periodic link-status check via the SDK HAL PHY functions
 *   (UpdateLink -> ParseLink -> AdjustLink on change), and the carrier
 *   notification the network stack requires: netdev_carrier_on/off()
 *   set/clear IFF_RUNNING, which netdev_findby_ripv4addr() filters on
 *   - without it every sendto() fails with ENETUNREACH even when the
 *   PHY has linked up (lan91c111.c netdev_carrier_on() pattern).
 *
 ****************************************************************************/

static void rk3506_gmac0_linkpoll_work(FAR void *arg)
{
  FAR struct rk3506_gmac0_s *priv = (FAR struct rk3506_gmac0_s *)arg;
  int oldlink;
  int ret;

  net_lock();

  oldlink = priv->gmac.phyStatus.link;

  ret = HAL_GMAC_PHYUpdateLink(&priv->gmac);
  if (ret == HAL_OK && priv->gmac.phyStatus.link != oldlink)
    {
      if (priv->gmac.phyStatus.link)
        {
          HAL_GMAC_PHYParseLink(&priv->gmac);
          HAL_GMAC_AdjustLink(&priv->gmac, 0, 0);

          /* Tell the network stack the device is operational
           * (IFF_RUNNING).  Without this, netdev_findby_ripv4addr()
           * skips the device and every sendto() fails with
           * ENETUNREACH even though the link is up.
           */

          netdev_carrier_on(&priv->dev);
        }
      else
        {
          netdev_carrier_off(&priv->dev);
        }

#ifdef CONFIG_NETDEV_PHY_IOCTL
      /* Wake the SIOCMIINOTIFY registrant (netinit monitor) so it can
       * react to the link transition immediately instead of waiting
       * for its poll timeout.
       */

      if (priv->notify_pid != 0 &&
          priv->notify_event.sigev_notify == SIGEV_SIGNAL)
        {
          nxsig_kill(priv->notify_pid, priv->notify_event.sigev_signo);
        }
#endif
    }

  /* Drain RX as a fallback for a dead IRQ path: with a healthy DMA
   * IRQ this finds an empty ring (one descriptor read) and costs
   * nothing.  Guarantees RX progresses as long as the link poll runs.
   */

  rk3506_gmac0_receive(priv);

  net_unlock();

  /* Re-arm the link poll */

  wd_start(&priv->linktimer, GMAC0_LINKPOLL,
           rk3506_gmac0_linkpoll_expiry, (wdparm_t)priv);
}

static void rk3506_gmac0_linkpoll_expiry(wdparm_t arg)
{
  FAR struct rk3506_gmac0_s *priv = (FAR struct rk3506_gmac0_s *)arg;

  work_queue(ETHWORK, &priv->pollwork, rk3506_gmac0_linkpoll_work,
             priv, 0);
}

/****************************************************************************
 * Name: rk3506_gmac0_interrupt_work
 *
 * Description:
 *   Deferred interrupt processing (dm90x0 pattern): dispatch the SDK
 *   HAL IRQ status bits to RX drain / TX done.
 *
 ****************************************************************************/

static void rk3506_gmac0_interrupt_work(FAR void *arg)
{
  FAR struct rk3506_gmac0_s *priv = (FAR struct rk3506_gmac0_s *)arg;
  enum eGMAC_IRQ_Status status;

  net_lock();

  status = HAL_GMAC_IRQHandler(&priv->gmac);

  if (status & DMA_HANLE_RX)
    {
      rk3506_gmac0_receive(priv);
    }

  if (status & DMA_HANLE_TX)
    {
      rk3506_gmac0_txdone(priv);
    }

  net_unlock();

  /* Re-enable the Ethernet interrupt (level-high GIC line) */

  up_enable_irq(RK3506_IRQ_GMAC0_0);
}

/****************************************************************************
 * Name: rk3506_gmac0_interrupt
 *
 * Description:
 *   Hardware interrupt handler: mask the IRQ line and defer the RX/TX
 *   processing to the work queue.
 *
 ****************************************************************************/

static int rk3506_gmac0_interrupt(int irq, FAR void *context,
                                  FAR void *arg)
{
  FAR struct rk3506_gmac0_s *priv = (FAR struct rk3506_gmac0_s *)arg;

  up_disable_irq(RK3506_IRQ_GMAC0_0);
  work_queue(ETHWORK, &priv->irqwork, rk3506_gmac0_interrupt_work,
             priv, 0);
  return OK;
}

/****************************************************************************
 * Name: rk3506_gmac0_txavail_work / rk3506_gmac0_txavail
 *
 * Description:
 *   New TX data callback (dm90x0 pattern): poll the network from the
 *   worker thread.
 *
 ****************************************************************************/

static void rk3506_gmac0_txavail_work(FAR void *arg)
{
  FAR struct rk3506_gmac0_s *priv = (FAR struct rk3506_gmac0_s *)arg;

  net_lock();
  if (priv->bifup)
    {
      devif_poll(&priv->dev, rk3506_gmac0_txpoll);
    }
  net_unlock();
}

static int rk3506_gmac0_txavail(FAR struct net_driver_s *dev)
{
  FAR struct rk3506_gmac0_s *priv =
    (FAR struct rk3506_gmac0_s *)dev->d_private;

  if (work_available(&priv->txavailwork))
    {
      work_queue(ETHWORK, &priv->txavailwork, rk3506_gmac0_txavail_work,
                 priv, 0);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3506_gmac0_ifup
 *
 * Description:
 *   NuttX callback: bring the interface up.  Sequence:
 *     CRU clocks -> RMII IOMUX -> GPIO PHY reset -> HAL_GMAC_Init
 *     (GRF + CSR + link table) -> descriptor rings -> HAL_GMAC_Start
 *     (SWR + DMA + MAC + rings) -> attach/enable IRQ -> PHY init
 *     (MDIO softreset + scan + AN config) -> PHY startup -> initial
 *     link adjust -> link poll timer.
 *
 ****************************************************************************/

static int rk3506_gmac0_ifup(FAR struct net_driver_s *dev)
{
  FAR struct rk3506_gmac0_s *priv =
    (FAR struct rk3506_gmac0_s *)dev->d_private;
  struct GMAC_PHY_Config config;
  int ret;
  int i;

  /* SoC/board glue (all verified in M2b.1 bring-up) */

  rk3506_gmac0_clock_init();
  rk3506_ioc_gmac0_rmii_setup();
  rk3506_gmac0_phy_reset();

  /* HAL init: RMII GRF config, CSR from the ~297 MHz MDIO clock
   * (hal_gmac.c HAL_GMAC_Init verbatim).  Clock direction per the
   * RK3506_GMAC0_EXTCLK choice: true = PHY drives REF_CLK into
   * GPIO2_B2 (GRF_SOC_CON8 bit5 = SELET_IO; the HD-RK3506-EVM,
   * board doc clock_in_out = "input"); false = CRU drives it out
   * (Rockchip in-house EVM "clock_in_out=output").
   */

  ret = HAL_GMAC_Init(&priv->gmac, RK3506_GMAC0_ADDR,
                      RK3506_GMAC0_MDIO_FREQ,
                      PHY_INTERFACE_MODE_RMII,
#ifdef CONFIG_RK3506_GMAC0_EXTCLK
                      true);
#else
                      false);
#endif
  if (ret != HAL_OK)
    {
      return ret;
    }

  /* Descriptor rings (HAL DMATx/RxDescInit verbatim). */

  HAL_GMAC_DMATxDescInit(&priv->gmac, g_tx_desc, g_tx_buf, ETH_TXBUFNB);
  HAL_GMAC_DMARxDescInit(&priv->gmac, g_rx_desc, g_rx_buf, ETH_RXBUFNB);

  up_clean_dcache((uintptr_t)g_tx_desc, (uintptr_t)g_tx_desc +
                  sizeof(g_tx_desc));
  up_clean_dcache((uintptr_t)g_rx_desc, (uintptr_t)g_rx_desc +
                  sizeof(g_rx_desc));

  /* Start the MAC (SWR + 100ms + DMA/MTL/MAC init + rings; hal_gmac.c
   * HAL_GMAC_Start verbatim).
   */

  ret = HAL_GMAC_Start(&priv->gmac, dev->d_mac.ether.ether_addr_octet);
  if (ret != HAL_OK)
    {
      return ret;
    }

  /* Attach and enable the MAC IRQ (GIC_SPI 66). */

  ret = irq_attach(RK3506_IRQ_GMAC0_0, rk3506_gmac0_interrupt, priv);
  if (ret != OK)
    {
      return ret;
    }

  up_enable_irq(RK3506_IRQ_GMAC0_0);

  /* PHY: HAL state machine (BMCR softreset, advertise, autoneg
   * restart) + the board PHY hook: Motorcomm YT8512B vendor config
   * (RMII mode + PLL refclk select, ported from the Linux SDK
   * motorcomm.c yt8512_config_init) - without it the PHY's MII side
   * does not speak RMII and every RX frame arrives corrupted.
   */

  memset(&priv->gmac.phyOps, 0, sizeof(priv->gmac.phyOps));
  priv->gmac.phyOps.init = rk3506_gmac0_yt8512b_init;

  config.interface = PHY_INTERFACE_MODE_RMII;

  /* Concrete address, like the SDK's own drv_gmac.c (it passes the
   * board's phy_addr, never -1): HAL_GMAC_PHYInit does the BMCR
   * softreset AT config->phyAddress BEFORE any scan, so -1 would
   * target PA=31 (empty -> 0xffff -> reset bit never clears ->
   * HAL_TIMEOUT).  M2b.1 verified our YT8512B sits at addr 0.
   */

  config.phyAddress = 0;
  config.neg = PHY_AUTONEG_ENABLE;
  config.speed = PHY_SPEED_100M;
  config.duplexMode = PHY_DUPLEX_FULL;
  config.maxSpeed = PHY_SPEED_100M;       /* RMII is 10/100 only */
  config.features = 0;                    /* default feature set */

  ret = HAL_GMAC_PHYInit(&priv->gmac, &config);
  if (ret != HAL_OK)
    {
      up_disable_irq(RK3506_IRQ_GMAC0_0);
      return ret;
    }

  HAL_GMAC_PHYStartup(&priv->gmac);

  /* Initial link bring-up: RMII autoneg needs a couple of seconds
   * after the PHY reset.  Wait for it here (bounded) so that the
   * interface is RUNNING (netdev_carrier_on) before ifup returns.
   * Boot-time DHCP depends on this: netdev_default() filters on
   * IFF_RUNNING for the broadcast DISCOVER, and dhcpc_request()
   * aborts on the first sendto failure with no retry - without the
   * wait, netinit's DHCP always loses the race against autoneg and
   * the board boots with no address.  On timeout (no cable) we fall
   * through to the link poll, which will carrier_on later; DHCP can
   * then be re-run with `renew eth0`.
   */

  for (i = 0; i < GMAC0_LINKWAIT_MS / 100; i++)
    {
      HAL_GMAC_PHYUpdateLink(&priv->gmac);
      if (priv->gmac.phyStatus.link)
        {
          break;
        }

      nxsig_usleep(100 * 1000);
    }

  if (priv->gmac.phyStatus.link)
    {
      HAL_GMAC_PHYParseLink(&priv->gmac);
      HAL_GMAC_AdjustLink(&priv->gmac, 0, 0);
      netdev_carrier_on(&priv->dev);
    }

  priv->bifup = true;

  /* Start the link poll watchdog */

  wd_start(&priv->linktimer, GMAC0_LINKPOLL,
           rk3506_gmac0_linkpoll_expiry, (wdparm_t)priv);

  return OK;
}

/****************************************************************************
 * Name: rk3506_gmac0_ifdown
 ****************************************************************************/

static int rk3506_gmac0_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct rk3506_gmac0_s *priv =
    (FAR struct rk3506_gmac0_s *)dev->d_private;
  irqstate_t flags;

  flags = enter_critical_section();

  priv->bifup = false;

  /* Stop the watchdogs and cancel deferred work */

  wd_cancel(&priv->txtimeout);
  wd_cancel(&priv->linktimer);
  work_cancel(ETHWORK, &priv->irqwork);
  work_cancel(ETHWORK, &priv->pollwork);
  work_cancel(ETHWORK, &priv->txavailwork);

  /* Mask the DMA IRQs, then the GIC line */

  HAL_GMAC_DisableDmaIRQ(&priv->gmac);
  up_disable_irq(RK3506_IRQ_GMAC0_0);

  /* Stop the MAC (hal_gmac.c HAL_GMAC_Stop verbatim) */

  HAL_GMAC_Stop(&priv->gmac);

  /* The interface is going down: clear IFF_RUNNING so the stack stops
   * routing through it (netdev_findby_ripv4addr filters on RUNNING).
   * Safe to call even if the carrier was never on.
   */

  netdev_carrier_off(dev);

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: rk3506_gmac0_addmac / rk3506_gmac0_rmmac
 *
 * Description:
 *   Multicast MAC filters.  The DWMAC4 MAC_PACKET_FILTER has 128
 *   perfect-filter slots; the SDK HAL does not wrap them, so for now
 *   multicast is accepted (the packet filter default passes it) and
 *   these are bookkeeping no-ops.
 *
 ****************************************************************************/

#ifdef CONFIG_NET_MCASTGROUP
static int rk3506_gmac0_addmac(FAR struct net_driver_s *dev,
                               FAR const uint8_t *mac)
{
  return OK;
}

static int rk3506_gmac0_rmmac(FAR struct net_driver_s *dev,
                              FAR const uint8_t *mac)
{
  return OK;
}
#endif

/****************************************************************************
 * Name: rk3506_gmac0_ioctl
 *
 * Description:
 *   PHY-status ioctls (CONFIG_NETDEV_PHY_IOCTL).  The netinit monitor
 *   (apps/netutils/netinit, CONFIG_NETINIT_MONITOR) uses these to
 *   track the link: SIOCGMIIPHY to get the PHY address, SIOCGMIIREG to
 *   poll the MII status register, and SIOCMIINOTIFY to register a
 *   sigevent that this driver fires on link transitions (from the
 *   link poll, which detects up/down changes every GMAC0_LINKPOLL).
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_PHY_IOCTL
static int rk3506_gmac0_ioctl(FAR struct net_driver_s *dev, int cmd,
                              unsigned long arg)
{
  FAR struct rk3506_gmac0_s *priv =
    (FAR struct rk3506_gmac0_s *)dev->d_private;
  int32_t val;
  int ret2;
  int ret = OK;

  /* The netdev core passes &ifr_ifru.ifru_mii_data for the MII register
   * commands and &ifr_ifru.ifru_mii_notify for SIOCMIINOTIFY (see
   * netdev_ioctl.c), NOT the full struct ifreq.
   */

  switch (cmd)
    {
    case SIOCGMIIPHY:               /* Get MII PHY address */
      {
        FAR struct mii_ioctl_data_s *mii =
          (FAR struct mii_ioctl_data_s *)((uintptr_t)arg);

        mii->phy_id = (uint16_t)priv->gmac.phyStatus.addr;
      }
      break;

    case SIOCGMIIREG:               /* Get MII register (MDIO read) */
      {
        FAR struct mii_ioctl_data_s *mii =
          (FAR struct mii_ioctl_data_s *)((uintptr_t)arg);

        /* MDIO transactions are not re-entrant: the link poll also
         * drives the PHY from under net_lock(), so hold it here too.
         */

        net_lock();
        val = HAL_GMAC_MDIORead(&priv->gmac, mii->phy_id, mii->reg_num);
        net_unlock();
        if (val < 0)
          {
            ret = -EIO;
          }
        else
          {
            mii->val_out = (uint16_t)val;
          }
      }
      break;

    case SIOCSMIIREG:               /* Set MII register (MDIO write) */
      {
        FAR struct mii_ioctl_data_s *mii =
          (FAR struct mii_ioctl_data_s *)((uintptr_t)arg);

        net_lock();
        ret2 = HAL_GMAC_MDIOWrite(&priv->gmac, mii->phy_id, mii->reg_num,
                                  mii->val_in);
        net_unlock();
        if (ret2 < 0)
          {
            ret = -EIO;
          }
      }
      break;

    case SIOCMIINOTIFY:             /* Register link-status notification */
      {
        FAR struct mii_ioctl_notify_s *notify =
          (FAR struct mii_ioctl_notify_s *)((uintptr_t)arg);

        /* pid == 0 means "the calling task".  Resolve it here while we
         * still run in the registrant's context (the netinit monitor),
         * because the signal is later sent from the link-poll work.
         */

        priv->notify_pid   = (notify->pid != 0) ? notify->pid :
                             (pid_t)getpid();
        priv->notify_event = notify->event;
      }
      break;

    default:

      ret = -ENOTTY;
      break;
    }

  return ret;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_gmac0_initialize
 *
 * Description:
 *   Register the GMAC0 Ethernet device with the network stack.  Called
 *   from the board bring-up.  Hardware power-up happens in ifup().
 *
 * Returned Value:
 *   OK on success; negated errno on failure.
 *
 ****************************************************************************/

int rk3506_gmac0_initialize(void)
{
  FAR struct rk3506_gmac0_s *priv = &g_gmac0;
  static const uint8_t mac[6] = GMAC0_DEFAULT_MAC;

  memset(priv, 0, sizeof(struct rk3506_gmac0_s));

  priv->gmac.base = RK3506_GMAC0_ADDR;

  /* Initialize the driver structure (dm90x0 pattern) */

  priv->dev.d_buf     = (FAR uint8_t *)g_pktbuf;
  priv->dev.d_ifup    = rk3506_gmac0_ifup;
  priv->dev.d_ifdown  = rk3506_gmac0_ifdown;
  priv->dev.d_txavail = rk3506_gmac0_txavail;
#ifdef CONFIG_NETDEV_PHY_IOCTL
  priv->dev.d_ioctl   = rk3506_gmac0_ioctl;
#endif
#ifdef CONFIG_NET_MCASTGROUP
  priv->dev.d_addmac  = rk3506_gmac0_addmac;
  priv->dev.d_rmmac   = rk3506_gmac0_rmmac;
#endif
  priv->dev.d_private = priv;

  /* MAC address (locally administered default) */

  memcpy(priv->dev.d_mac.ether.ether_addr_octet, mac, 6);

  netdev_register(&priv->dev, NET_LL_ETHERNET);

  return OK;
}

#endif /* CONFIG_RK3506_GMAC0 */
