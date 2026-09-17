/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_gmac_hal.c
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Verbatim NuttX port of the Rockchip SDK GMAC HAL for the Synopsys
 * DWMAC 4.20a core:
 *   hal/lib/hal/src/gmac/hal_gmac.c         (core driver)
 *   hal/lib/hal/src/gmac/hal_gmac_rk3506.c  (RK3506 GRF glue)
 *
 * Every register sequence, bit field and state machine below is kept
 * identical to the SDK source.  Adaptations (documented inline):
 *   - register access: base + GMAC_*_OFFSET from rk3506.h via
 *     getreg32/putreg32 instead of the packed struct GMAC_REG;
 *   - HAL_DelayUs/Ms / HAL_GetTick: nxsched_usleep / clock();
 *   - HAL Malloc/Free: not used (caller provides static rings);
 *   - PTP: disabled (HAL_GMAC_PTP_FEATURE_ENABLED not defined in the
 *     SDK build either).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <syslog.h>
#include <time.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_gmac_hal.h"

#ifndef putreg32
#  define putreg32(v, a) (*(FAR volatile uint32_t *)(a) = (v))
#endif
#ifndef getreg32
#  define getreg32(a)    (*(FAR volatile uint32_t *)(a))
#endif


/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets (rk3506.h GMAC_*_OFFSET) */

#define GMAC_MAC_CONFIGURATION          0x0000u
#define GMAC_MAC_PACKET_FILTER          0x0008u
#define GMAC_MAC_Q0_TX_FLOW_CTRL        0x0070u
#define GMAC_MAC_RX_FLOW_CTRL           0x0090u
#define GMAC_MAC_VERSION                0x0110u
#define GMAC_MAC_HW_FEATURE1            0x0120u
#define GMAC_MAC_MDIO_ADDRESS           0x0200u
#define GMAC_MAC_MDIO_DATA              0x0204u
#define GMAC_MAC_ADDRESS0_HIGH          0x0300u
#define GMAC_MAC_ADDRESS0_LOW           0x0304u
#define GMAC_MMC_CONTROL                0x0700u
#define GMAC_MMC_RX_INTERRUPT_MASK      0x070cu
#define GMAC_MMC_TX_INTERRUPT_MASK      0x0710u
#define GMAC_MMC_IPC_RX_INTERRUPT_MASK  0x0800u

/* MMC hardware counters: defined in rk3506_gmac_hal.h (shared with
 * the netdev bring-up statistics print).
 */

#define GMAC_MTL_TXQ0_OPERATION_MODE    0x0d00u
#define GMAC_MTL_Q0_INTERRUPT_CTRL_STAT 0x0d2cu
#define GMAC_MTL_RXQ0_OPERATION_MODE    0x0d30u
#define GMAC_DMA_MODE                   0x1000u
#define GMAC_DMA_SYSBUS_MODE            0x1004u
#define GMAC_DMA_CH0_TX_CONTROL         0x1104u
#define GMAC_DMA_CH0_RX_CONTROL         0x1108u
#define GMAC_DMA_CH0_TXDESC_LIST_ADDR   0x1114u
#define GMAC_DMA_CH0_RXDESC_LIST_ADDR   0x111cu
#define GMAC_DMA_CH0_TXDESC_TAIL_PTR    0x1120u
#define GMAC_DMA_CH0_RXDESC_TAIL_PTR    0x1128u
#define GMAC_DMA_CH0_TXDESC_RING_LEN    0x112cu
#define GMAC_DMA_CH0_RXDESC_RING_LEN    0x1130u
#define GMAC_DMA_CH0_INTERRUPT_ENABLE   0x1134u
#define GMAC_DMA_CH0_STATUS             0x1160u

/* MDIO address register bits (rk3506.h GMAC_MAC_MDIO_ADDRESS_*) */

#define GMAC_MDIO_ADDRESS_GB_SHIFT      (0)
#define GMAC_MDIO_ADDRESS_GB            (0x1u << 0)
#define GMAC_MDIO_ADDRESS_C45E          (0x1u << 1)
#define GMAC_MDIO_ADDRESS_GOC_0_SHIFT   (2)
#define GMAC_MDIO_ADDRESS_GOC_READ      (0x3u << 2)
#define GMAC_MDIO_ADDRESS_GOC_WRITE     (0x1u << 2)
#define GMAC_MDIO_ADDRESS_SKAP          (0x1u << 4)
#define GMAC_MDIO_ADDRESS_CR_SHIFT      (8)
#define GMAC_MDIO_ADDRESS_RDA_SHIFT     (16)
#define GMAC_MDIO_ADDRESS_PA_SHIFT      (21)
#define GMAC_MDIO_DATA_GD_MASK          0xffffu

/* MDC clock range table (hal_gmac.c) */

#define GMAC_CSR_60_100M  0x0u /* MDC = clk_csr_i/42  */
#define GMAC_CSR_100_150M 0x1u /* MDC = clk_csr_i/62  */
#define GMAC_CSR_20_35M   0x2u /* MDC = clk_csr_i/16  */
#define GMAC_CSR_35_60M   0x3u /* MDC = clk_csr_i/26  */
#define GMAC_CSR_150_250M 0x4u /* MDC = clk_csr_i/102 */
#define GMAC_CSR_250_300M 0x5u /* MDC = clk_csr_i/122 */

#define GMAC_MDIO_TIMEOUT 100 /* ms */

/* MAC configuration (dwmac4.h, verbatim from hal_gmac.c) */

#define GMAC_CONFIG_GPSLCE (1u << 23)
#define GMAC_CONFIG_IPC    (1u << 27)
#define GMAC_CONFIG_2K     (1u << 22)
#define GMAC_CONFIG_CST    (1u << 21)
#define GMAC_CONFIG_ACS    (1u << 20)
#define GMAC_CONFIG_WD     (1u << 19)
#define GMAC_CONFIG_BE     (1u << 18)
#define GMAC_CONFIG_JD     (1u << 17)
#define GMAC_CONFIG_JE     (1u << 16)
#define GMAC_CONFIG_PS     (1u << 15)
#define GMAC_CONFIG_FES    (1u << 14)
#define GMAC_CONFIG_DM     (1u << 13)
#define GMAC_CONFIG_DCRS   (1u << 9)
#define GMAC_CONFIG_TE     (1u << 1)
#define GMAC_CONFIG_RE     (1u << 0)

#define GMAC_CORE_INIT (GMAC_CONFIG_JD | GMAC_CONFIG_PS | \
                        GMAC_CONFIG_ACS | GMAC_CONFIG_BE | \
                        GMAC_CONFIG_DCRS)

/* DMA registers (hal_gmac.c) */

#define DMA_MODE_SWR                (0x1u << 0)
#define DMA_SYSBUS_MODE_BLEN16      (1u << 3)
#define DMA_SYSBUS_MODE_BLEN8       (1u << 2)
#define DMA_SYSBUS_MODE_BLEN4       (1u << 1)
#define DMA_CHAN_STATUS_NIS         (0x1u << 15)
#define DMA_CHAN_STATUS_AIS         (0x1u << 14)
#define DMA_CHAN_STATUS_FBE         (0x1u << 12)
#define DMA_CHAN_STATUS_CDE         (0x1u << 13)
#define DMA_CHAN_STATUS_ETI         (0x1u << 10)
#define DMA_CHAN_STATUS_ERI         (0x1u << 11)
#define DMA_CHAN_STATUS_RWT         (0x1u << 9)
#define DMA_CHAN_STATUS_RPS         (0x1u << 8)
#define DMA_CHAN_STATUS_RBU         (0x1u << 7)
#define DMA_CHAN_STATUS_RI          (0x1u << 6)
#define DMA_CHAN_STATUS_TBU         (0x1u << 2)
#define DMA_CHAN_STATUS_TPS         (0x1u << 1)
#define DMA_CHAN_STATUS_TI          (0x1u << 0)
#define DMA_CHAN_INTR_ENA_NIE       (0x1u << 15)
#define DMA_CHAN_INTR_ENA_AIE       (0x1u << 14)
#define DMA_CHAN_INTR_ENA_CDE       (0x1u << 13)
#define DMA_CHAN_INTR_ENA_FBE       (0x1u << 12)
#define DMA_CHAN_INTR_ENA_ERE       (0x1u << 11)
#define DMA_CHAN_INTR_ENA_ETE       (0x1u << 10)
#define DMA_CHAN_INTR_ENA_RWE       (0x1u << 9)
#define DMA_CHAN_INTR_ENA_RSE       (0x1u << 8)
#define DMA_CHAN_INTR_ENA_RBUE      (0x1u << 7)
#define DMA_CHAN_INTR_ENA_RIE       (0x1u << 6)
#define DMA_CHAN_INTR_ENA_TBUE      (0x1u << 2)
#define DMA_CHAN_INTR_ENA_TSE       (0x1u << 1)
#define DMA_CHAN_INTR_ENA_TIE       (0x1u << 0)
#define DMA_CHAN_INTR_NORMAL        (DMA_CHAN_INTR_ENA_NIE | \
                                     DMA_CHAN_INTR_ENA_RIE | \
                                     DMA_CHAN_INTR_ENA_TIE)
#define DMA_CHAN_INTR_ABNORMAL      (DMA_CHAN_INTR_ENA_AIE | \
                                     DMA_CHAN_INTR_ENA_FBE)
#define DMA_CHAN_INTR_DEFAULT_MASK  (DMA_CHAN_INTR_NORMAL | \
                                     DMA_CHAN_INTR_ABNORMAL)

#define DMA_CH0_RX_CONTROL_RBSZ_SHIFT  (1)
/* RBSZ field = bits[14:1] (rk3506.h RBSZ_3_0 0xE | RBSZ_13_Y 0x7FF0).
 * NOTE: hal_gmac.c uses mask 0x3fff (bits[13:0] only); with the
 * RK3506's 8 KiB RX FIFO that truncates 8192<<1=0x4000 to 0 and the
 * RX DMA drops every frame (seen on hardware: rxctl=0x00080001,
 * rbsz=0).  Use the full field mask and an explicit buffer size. */
#define DMA_CH0_RX_CONTROL_RBSZ_MASK   (0x7ffeu)
#define DMA_CH0_RX_CONTROL_RXPBL_SHIFT (16)
#define DMA_CH0_TX_CONTROL_TXPBL_SHIFT (16)
#define DMA_CH0_TX_CONTROL_OSF         (0x1u << 4)
#define DMA_CH0_TX_CONTROL_ST          (0x1u << 0)
#define DMA_CH0_RX_CONTROL_SR          (0x1u << 0)

/* MTL (hal_gmac.c) */

#define MTL_RXQ0_OPERATION_MODE_EHFC (1u << 7)
#define MTL_RXQ0_OPERATION_MODE_RSF  (1u << 5)
#define MTL_RXQ0_OPERATION_MODE_FEP  (1u << 6)
#define MTL_RXQ0_OPERATION_MODE_FUP  (1u << 4)
#define MTL_OP_MODE_TXQEN_MASK       (3u << 2)
#define MTL_OP_MODE_TXQEN_AV         (1u << 2)
#define MTL_OP_MODE_TXQEN            (1u << 3)
#define MTL_OP_MODE_TSF              (1u << 1)
#define MTL_OP_MODE_TQS_MASK         (0x1fu << 16)
#define MTL_OP_MODE_TQS_SHIFT        (16)
#define MTL_OP_MODE_TTC_MASK         (0x7u << 4)
#define MTL_OP_MODE_TTC_SHIFT        (4)
#define MTL_OP_MODE_TTC_32           0
#define MTL_OP_MODE_TTC_64           (1u << MTL_OP_MODE_TTC_SHIFT)
#define MTL_OP_MODE_TTC_96           (2u << MTL_OP_MODE_TTC_SHIFT)
#define MTL_OP_MODE_TTC_128          (3u << MTL_OP_MODE_TTC_SHIFT)
#define MTL_OP_MODE_TTC_192          (4u << MTL_OP_MODE_TTC_SHIFT)
#define MTL_OP_MODE_TTC_256          (5u << MTL_OP_MODE_TTC_SHIFT)
#define MTL_OP_MODE_TTC_384          (6u << MTL_OP_MODE_TTC_SHIFT)
#define MTL_OP_MODE_TTC_512          (7u << MTL_OP_MODE_TTC_SHIFT)
#define MTL_OP_MODE_RQS_SHIFT        (20)
#define MTL_OP_MODE_RQS_MASK         (0x3ffu << 20)
#define MTL_OP_MODE_RFD_SHIFT        (14)
#define MTL_OP_MODE_RFD_MASK         (0x3fu << 14)
#define MTL_OP_MODE_RFA_SHIFT        (8)
#define MTL_OP_MODE_RFA_MASK         (0x3fu << 8)
#define MTL_OP_MODE_RTC_SHIFT        (3)
#define MTL_OP_MODE_RTC_MASK         (0x3u << 3)
#define MTL_OP_MODE_RTC_32           (1u << 3)
#define MTL_OP_MODE_RTC_64           0
#define MTL_OP_MODE_RTC_96           (2u << 3)
#define MTL_OP_MODE_RTC_128          (3u << 3)

#define SF_DMA_MODE 1 /* DMA STORE-AND-FORWARD Operation Mode */

/* MTL interrupt (hal_gmac.c) */

#define MTL_RX_OVERFLOW_INT_EN (1u << 24)
#define MTL_RX_OVERFLOW_INT    (1u << 16)

/* MAC address filter / packet filter (Linux dwmac4.h, rk3506.h):
 * AE (MAC_ADDRESS0_HIGH bit31) MUST be set or the perfect filter never
 * matches and every unicast frame addressed to us is dropped (the SDK
 * HAL omits it).  PM (PACKET_FILTER bit4) passes all multicast, needed
 * for IPv6 NDP since the NuttX driver's addmac is a no-op.
 */

#define GMAC_ADDR_HIGH_AE            (0x1u << 31)
#define GMAC_PACKET_FILTER_PM        (0x1u << 4)

/* Flow control (rk3506.h) */

#define GMAC_RX_FLOW_CTRL_RFE       (0x1u << 0)
#define GMAC_Q0_TX_FLOW_CTRL_TFE    (0x1u << 1)
#define GMAC_Q0_TX_FLOW_CTRL_PT_SHIFT (16)
#define HAL_PAUSE_TIME              0xffff

/* Description related (hal_gmac.c) */

#define GMAC_DESC3_OWN   (0x1u << 31)
#define GMAC_DESC3_IOC   (0x1u << 30)
#define GMAC_DESC3_FD    (0x1u << 29)
#define GMAC_DESC3_LD    (0x1u << 28)
#define GMAC_DESC3_BUF1V (0x1u << 24)
#define GMAC_DESC3_CIC   (0x3u << 16)
#define DES3_ERROR_SUMMARY (1u << 15)
#define RDES3_OWN (1u << 31)
#define ETH_FCS_LEN 4

/* MMC (hal_gmac.c) */

#define MMC_CNTRL_RESET_ON_READ     (0x1u << 2)
#define MMC_CNTRL_COUNTER_RESET     (0x1u << 0)
#define MMC_CNTRL_PRESET            (0x1u << 4)
#define MMC_CNTRL_FULL_HALF_PRESET  (0x1u << 5)
#define MMC_DEFAULT_MASK            0xffffffffu

/* HW feature fifo sizes (hal_gmac.c) */

#define GMAC_HW_TXFIFOSIZE_SHIFT (6)  /* rk3506.h GMAC_MAC_HW_FEATURE1_TXFIFOSIZE_SHIFT */
#define GMAC_HW_RXFIFOSIZE_SHIFT (0)  /* rk3506.h GMAC_MAC_HW_FEATURE1_RXFIFOSIZE_SHIFT */
#define GMAC_HW_TXFIFOSIZE       (0x1fu << GMAC_HW_TXFIFOSIZE_SHIFT)
#define GMAC_HW_RXFIFOSIZE       (0x1fu << GMAC_HW_RXFIFOSIZE_SHIFT)

#define GMAC_GET_ENTRY(x, size) ((x + 1) & (size - 1))

/* Generic MII registers (hal_gmac.c) */

#define MII_BMCR        0x00
#define MII_BMSR        0x01
#define MII_PHYSID1     0x02
#define MII_PHYSID2     0x03
#define MII_ADVERTISE   0x04
#define MII_LPA         0x05
#define MII_EXPANSION   0x06
#define MII_CTRL1000    0x09
#define MII_STAT1000    0x0a
#define MII_ESTATUS     0x0f

/* Basic mode control register (hal_gmac.c) */

#define BMCR_SPEED1000 0x0040
#define BMCR_CTST      0x0080
#define BMCR_FULLDPLX  0x0100
#define BMCR_ANRESTART 0x0200
#define BMCR_ISOLATE   0x0400
#define BMCR_PDOWN     0x0800
#define BMCR_ANENABLE  0x1000
#define BMCR_SPEED100  0x2000
#define BMCR_LOOPBACK  0x4000
#define BMCR_RESET     0x8000
#define BMCR_SPEED10   0x0000

/* Basic mode status register (hal_gmac.c) */

#define BMSR_ERCAP        0x0001
#define BMSR_JCD          0x0002
#define BMSR_LSTATUS      0x0004
#define BMSR_ANEGCAPABLE  0x0008
#define BMSR_RFAULT       0x0010
#define BMSR_ANEGCOMPLETE 0x0020
#define BMSR_ESTATEN      0x0100
#define BMSR_10HALF       0x0800
#define BMSR_10FULL       0x1000
#define BMSR_100HALF      0x2000
#define BMSR_100FULL      0x4000
#define BMSR_100BASE4     0x8000

/* Advertisement control register (hal_gmac.c) */

#define ADVERTISE_SLCT          0x001f
#define ADVERTISE_CSMA          0x0001
#define ADVERTISE_10HALF        0x0020
#define ADVERTISE_1000XFULL     0x0020
#define ADVERTISE_10FULL        0x0040
#define ADVERTISE_1000XHALF     0x0040
#define ADVERTISE_100HALF       0x0080
#define ADVERTISE_1000XPAUSE    0x0080
#define ADVERTISE_100FULL       0x0100
#define ADVERTISE_1000XPSE_ASYM 0x0100
#define ADVERTISE_100BASE4      0x0200
#define ADVERTISE_PAUSE_CAP     0x0400
#define ADVERTISE_PAUSE_ASYM    0x0800
#define ADVERTISE_RESV          0x1000
#define ADVERTISE_RFAULT        0x2000
#define ADVERTISE_LPACK         0x4000
#define ADVERTISE_NPAGE         0x8000

#define ADVERTISE_FULL (ADVERTISE_100FULL | ADVERTISE_10FULL | \
                        ADVERTISE_CSMA)
#define ADVERTISE_ALL  (ADVERTISE_10HALF | ADVERTISE_10FULL | \
                        ADVERTISE_100HALF | ADVERTISE_100FULL)

/* Link partner ability register (hal_gmac.c) */

#define LPA_10HALF          0x0020
#define LPA_10FULL          0x0040
#define LPA_100HALF         0x0080
#define LPA_100FULL         0x0100
#define LPA_100BASE4        0x0200
#define LPA_PAUSE_CAP       0x0400
#define LPA_PAUSE_ASYM      0x0800
#define LPA_RFAULT          0x2000
#define LPA_LPACK           0x4000
#define LPA_NPAGE           0x8000

/* ESTATUS (hal_gmac.c) */

#define ESTATUS_1000_XFULL 0x8000
#define ESTATUS_1000_XHALF 0x4000
#define ESTATUS_1000_TFULL 0x2000
#define ESTATUS_1000_THALF 0x1000

/* 1000BASE-T control / status (hal_gmac.c) */

#define ADVERTISE_1000FULL    0x0200
#define ADVERTISE_1000HALF    0x0100
#define PHY_1000BTSR_1000FD 0x0800
#define PHY_1000BTSR_1000HD 0x0400

/* RK3506 GRF (hal_gmac_rk3506.c) */

#define GRF_BIT(nr)     (1u << (nr) | 1u << ((nr) + 16))
#define GRF_CLR_BIT(nr) (1u << ((nr) + 16))

#define RK3506_MAC_CLK_RMII_MODE  GRF_BIT(1)
#define RK3506_MAC_CLK_RMII_GATE  GRF_BIT(2)
#define RK3506_MAC_CLK_RMII_NOGATE GRF_CLR_BIT(2)
#define RK3506_MAC_CLK_RMII_DIV2  GRF_BIT(3)
#define RK3506_MAC_CLK_RMII_DIV20 GRF_CLR_BIT(3)
#define RK3506_MAC_CLK_SELET_CRU  GRF_CLR_BIT(5)
#define RK3506_MAC_CLK_SELET_IO   GRF_BIT(5)

#define RK3506_GRF_SOC_CON8 0x20u /* GRF + 0x20 */

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int PHY_GetID(struct GMAC_HANDLE *pGMAC, int32_t addr,
                     uint32_t *phyID);
static int PHY_InfoCreate(struct GMAC_HANDLE *pGMAC,
                          struct GMAC_PHY_Config *config,
                          int32_t addr, uint32_t id);
static int PHY_Connect(struct GMAC_HANDLE *pGMAC,
                       struct GMAC_PHY_Config *config, int32_t addr);
static int PHY_SetupForced(struct GMAC_HANDLE *pGMAC);
static int32_t PHY_ConfigAdvert(struct GMAC_HANDLE *pGMAC);
static int PHY_RestartAneg(struct GMAC_HANDLE *pGMAC);
static int PHY_ConfigAneg(struct GMAC_HANDLE *pGMAC);
static int PHY_Setsupported(struct GMAC_HANDLE *pGMAC, uint32_t maxSpeed);
static int PHY_Config(struct GMAC_HANDLE *pGMAC);
static int GMAC_FlowCtrl(struct GMAC_HANDLE *pGMAC, int32_t duplex,
                         uint32_t fc, uint32_t pauseTime);
static int GMAC_DMARXOpMode(struct GMAC_HANDLE *pGMAC, int32_t mode,
                            int32_t fifosz);
static int GMAC_DMATXOpMode(struct GMAC_HANDLE *pGMAC, int32_t mode,
                            int32_t fifosz);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* RK3506 MAC0 HW info (hal/lib/bsp/RK3506/hal_bsp.c g_gmac0Dev) */

/****************************************************************************
 * Name: HAL_DelayUs / HAL_DelayMs
 ****************************************************************************/

static void HAL_DelayUs(uint32_t us)
{
  up_udelay(us);
}

static void HAL_DelayMs(uint32_t ms)
{
  up_udelay(ms * 1000u);
}

/****************************************************************************
 * Name: HAL_GetTick
 *
 * Description:
 *   Millisecond tick, used only by Mdio_WaitIdle().
 *
 ****************************************************************************/

static uint32_t HAL_GetTick(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/****************************************************************************
 * Name: Mdio_WaitIdle
 *
 * Description:
 *   Wait for Mdio to idle state (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int Mdio_WaitIdle(struct GMAC_HANDLE *pGMAC)
{
  uint32_t tickstart = 0;

  /* Get tick */
  tickstart = HAL_GetTick();

  while ((getreg32(pGMAC->base + GMAC_MAC_MDIO_ADDRESS) &
          GMAC_MDIO_ADDRESS_GB) == GMAC_MDIO_ADDRESS_GB)
    {
      /* Check for the Timeout */

      if ((HAL_GetTick() - tickstart) > GMAC_MDIO_TIMEOUT)
        {
          return HAL_TIMEOUT;
        }
    }

  return HAL_OK;
}

/****************************************************************************
 * Name: PHY_GetID
 *
 * Description:
 *   Reads the PHY ID by specified addr (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int PHY_GetID(struct GMAC_HANDLE *pGMAC, int32_t addr,
                     uint32_t *phyID)
{
  int32_t phyReg;

  /* Grab the bits from PHYIR1, and put them in the upper half. */

  phyReg = HAL_GMAC_MDIORead(pGMAC, addr, MII_PHYSID1);
  if (phyReg < 0)
    {
      return phyReg;
    }

  *phyID = (phyReg & 0xffff) << 16;

  /* Grab the bits from PHYIR2, and put them in the lower half */

  phyReg = HAL_GMAC_MDIORead(pGMAC, addr, MII_PHYSID2);
  if (phyReg < 0)
    {
      return phyReg;
    }

  *phyID |= (phyReg & 0xffff);

  return HAL_OK;
}

/****************************************************************************
 * Name: PHY_InfoCreate
 *
 * Description:
 *   Store the PHY status (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int PHY_InfoCreate(struct GMAC_HANDLE *pGMAC,
                          struct GMAC_PHY_Config *config,
                          int32_t addr, uint32_t id)
{
  pGMAC->phyStatus.link = 0;
  pGMAC->phyStatus.duplex = config->duplexMode;
  pGMAC->phyStatus.interface = config->interface;
  pGMAC->phyStatus.neg = config->neg;
  pGMAC->phyStatus.speed = config->speed;
  pGMAC->phyStatus.maxSpeed = config->maxSpeed;

  pGMAC->phyStatus.addr = addr;
  pGMAC->phyStatus.phyID = id;

  return HAL_OK;
}

/****************************************************************************
 * Name: PHY_Connect
 *
 * Description:
 *   Connect the PHY by address (hal_gmac.c verbatim).  Accepts any PHY
 *   whose ID is not 0xffffffff / 0x0000ffff-masked all-ones; the board's
 *   RTL8201 (ID 0x00000128, ID1==0) is accepted.
 *
 ****************************************************************************/

static int PHY_Connect(struct GMAC_HANDLE *pGMAC,
                       struct GMAC_PHY_Config *config, int32_t addr)
{
  int status;
  uint32_t phyID = 0xffffffff;

  status = PHY_GetID(pGMAC, addr, &phyID);
  if (status == HAL_OK && (phyID & 0x1fffffff) != 0x1fffffff)
    {
      return PHY_InfoCreate(pGMAC, config, addr, phyID);
    }

  return HAL_NODEV;
}

/****************************************************************************
 * Name: PHY_SetupForced
 *
 * Description:
 *   Configures/forces speed/duplex from PHY status (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int PHY_SetupForced(struct GMAC_HANDLE *pGMAC)
{
  int32_t ctl = BMCR_ANRESTART;

  if (PHY_SPEED_1000M == pGMAC->phyStatus.speed)
    {
      ctl |= BMCR_SPEED1000;
    }
  else if (PHY_SPEED_100M == pGMAC->phyStatus.speed)
    {
      ctl |= BMCR_SPEED100;
    }

  if (PHY_DUPLEX_FULL == pGMAC->phyStatus.duplex)
    {
      ctl |= BMCR_FULLDPLX;
    }

  return HAL_GMAC_MDIOWrite(pGMAC, pGMAC->phyStatus.addr, MII_BMCR, ctl);
}

/****************************************************************************
 * Name: PHY_ConfigAdvert
 *
 * Description:
 *   Configures PHY advert features (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int32_t PHY_ConfigAdvert(struct GMAC_HANDLE *pGMAC)
{
  uint32_t adverTise;
  int32_t oldAdv, adv, bmsr;
  int32_t err, changed = 0;

  /* Only allow advertising what this PHY supports */

  pGMAC->phyStatus.advertising &= pGMAC->phyStatus.supported;
  adverTise = pGMAC->phyStatus.advertising;

  /* Setup standard advertisement */

  adv = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_ADVERTISE);
  if (adv < 0)
    {
      return adv;
    }

  oldAdv = adv;
  adv &= ~(ADVERTISE_ALL | ADVERTISE_100BASE4 | ADVERTISE_PAUSE_CAP |
           ADVERTISE_PAUSE_ASYM);
  if (adverTise & HAL_GMAC_PHY_SUPPORTED_10baseT_Half)
    {
      adv |= ADVERTISE_10HALF;
    }
  if (adverTise & HAL_GMAC_PHY_SUPPORTED_10baseT_Full)
    {
      adv |= ADVERTISE_10FULL;
    }
  if (adverTise & HAL_GMAC_PHY_SUPPORTED_100baseT_Half)
    {
      adv |= ADVERTISE_100HALF;
    }
  if (adverTise & HAL_GMAC_PHY_SUPPORTED_100baseT_Full)
    {
      adv |= ADVERTISE_100FULL;
    }
  if (adverTise & HAL_GMAC_PHY_SUPPORTED_Pause)
    {
      adv |= ADVERTISE_PAUSE_CAP;
    }
  if (adverTise & HAL_GMAC_PHY_SUPPORTED_Asym_Pause)
    {
      adv |= ADVERTISE_PAUSE_ASYM;
    }

  if (adv != oldAdv)
    {
      err = HAL_GMAC_MDIOWrite(pGMAC, pGMAC->phyStatus.addr,
                               MII_ADVERTISE, adv);
      if (err < 0)
        {
          return err;
        }

      changed = 1;
    }

  bmsr = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_BMSR);
  if (bmsr < 0)
    {
      return bmsr;
    }

  /* Per 802.3-2008, Section 22.2.4.2.16 Extended status: all 1000Mbits
   * capable PHYs shall have the BMSR_ESTATEN bit set to a logical 1.
   */

  if (!(bmsr & BMSR_ESTATEN))
    {
      return changed;
    }

  /* Configure gigabit if it's supported */

  adv = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_CTRL1000);
  if (adv < 0)
    {
      return adv;
    }

  oldAdv = adv;
  adv &= ~(ADVERTISE_1000FULL | ADVERTISE_1000HALF);

  if (pGMAC->phyStatus.supported &
      (HAL_GMAC_PHY_SUPPORTED_1000baseT_Half |
       HAL_GMAC_PHY_SUPPORTED_1000baseT_Full))
    {
      if (adverTise & HAL_GMAC_PHY_SUPPORTED_1000baseT_Half)
        {
          adv |= ADVERTISE_1000HALF;
        }
      if (adverTise & HAL_GMAC_PHY_SUPPORTED_1000baseT_Full)
        {
          adv |= ADVERTISE_1000FULL;
        }
    }

  if (adv != oldAdv)
    {
      changed = 1;
    }

  err = HAL_GMAC_MDIOWrite(pGMAC, pGMAC->phyStatus.addr, MII_CTRL1000, adv);
  if (err < 0)
    {
      return err;
    }

  return changed;
}

/****************************************************************************
 * Name: PHY_RestartAneg
 *
 * Description:
 *   Enable and Restart auto-negotiation (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int PHY_RestartAneg(struct GMAC_HANDLE *pGMAC)
{
  int32_t ctl;

  ctl = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_BMCR);
  if (ctl < 0)
    {
      return ctl;
    }

  ctl |= (BMCR_ANENABLE | BMCR_ANRESTART);

  /* Don't isolate the PHY if we're negotiating */

  ctl &= ~(BMCR_ISOLATE);
  ctl = HAL_GMAC_MDIOWrite(pGMAC, pGMAC->phyStatus.addr, MII_BMCR, ctl);

  return ctl;
}

/****************************************************************************
 * Name: PHY_ConfigAneg
 *
 * Description:
 *   If auto-negotiation is enabled, configure the advertising and
 *   restart auto-negotiation (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int PHY_ConfigAneg(struct GMAC_HANDLE *pGMAC)
{
  int result;

  if (PHY_AUTONEG_ENABLE != pGMAC->phyStatus.neg)
    {
      return PHY_SetupForced(pGMAC);
    }

  result = PHY_ConfigAdvert(pGMAC);
  if (result < 0)
    { /* error */
      return result;
    }

  if (result == 0)
    {
      /* Advertisement hasn't changed, but maybe aneg was never on to
       * begin with? Or maybe phy was isolated?
       */

      int32_t ctl = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr,
                                      MII_BMCR);
      if (ctl < 0)
        {
          return ctl;
        }

      if (!(ctl & BMCR_ANENABLE) || (ctl & BMCR_ISOLATE))
        {
          result = 1; /* do restart aneg */
        }
    }

  /* Only restart aneg if we are advertising something different than
   * we were before.
   */

  if (result > 0)
    {
      result = PHY_RestartAneg(pGMAC);
    }

  return result;
}

/****************************************************************************
 * Name: PHY_Setsupported
 *
 * Description:
 *   Set PHY max speed if provided (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int PHY_Setsupported(struct GMAC_HANDLE *pGMAC, uint32_t maxSpeed)
{
  pGMAC->phyStatus.supported &= HAL_GMAC_PHY_DEFAULT_FEATURES;

  switch (maxSpeed)
    {
    default:
      return HAL_ERROR;
    case PHY_SPEED_1000M:
      pGMAC->phyStatus.supported |= HAL_GMAC_PHY_SUPPORTED_1000baseT_Half |
                                    HAL_GMAC_PHY_SUPPORTED_1000baseT_Full;
      /* fall through */
    case PHY_SPEED_100M:
      pGMAC->phyStatus.supported |= HAL_GMAC_PHY_100BT_FEATURES;
      /* fall through */
    case PHY_SPEED_10M:
      pGMAC->phyStatus.supported |= HAL_GMAC_PHY_10BT_FEATURES;
    }

  return HAL_OK;
}

/****************************************************************************
 * Name: PHY_Config
 *
 * Description:
 *   Configure PHY by features (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int PHY_Config(struct GMAC_HANDLE *pGMAC)
{
  int32_t val = 0;
  uint32_t features;
  int status;

  if (pGMAC->phyOps.config)
    {
      status = pGMAC->phyOps.config(pGMAC);
      if (status)
        {
          return status;
        }
    }

  features = (HAL_GMAC_PHY_SUPPORTED_TP | HAL_GMAC_PHY_SUPPORTED_MII |
              HAL_GMAC_PHY_SUPPORTED_AUI | HAL_GMAC_PHY_SUPPORTED_FIBRE |
              HAL_GMAC_PHY_SUPPORTED_BNC);

  /* Do we support autonegotiation? */

  val = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_BMSR);
  if (val < 0)
    {
      return val;
    }

  if (val & BMSR_ANEGCAPABLE)
    {
      features |= HAL_GMAC_PHY_SUPPORTED_Autoneg;
    }
  if (val & BMSR_100FULL)
    {
      features |= HAL_GMAC_PHY_SUPPORTED_100baseT_Full;
    }
  if (val & BMSR_100HALF)
    {
      features |= HAL_GMAC_PHY_SUPPORTED_100baseT_Half;
    }
  if (val & BMSR_10FULL)
    {
      features |= HAL_GMAC_PHY_SUPPORTED_10baseT_Full;
    }
  if (val & BMSR_10HALF)
    {
      features |= HAL_GMAC_PHY_SUPPORTED_10baseT_Half;
    }

  if (val & BMSR_ESTATEN)
    {
      val = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_ESTATUS);
      if (val < 0)
        {
          return val;
        }

      if (val & ESTATUS_1000_TFULL)
        {
          features |= HAL_GMAC_PHY_SUPPORTED_1000baseT_Full;
        }
      if (val & ESTATUS_1000_THALF)
        {
          features |= HAL_GMAC_PHY_SUPPORTED_1000baseT_Half;
        }
      if (val & ESTATUS_1000_XFULL)
        {
          features |= HAL_GMAC_PHY_SUPPORTED_1000baseX_Full;
        }
      if (val & ESTATUS_1000_XHALF)
        {
          features |= HAL_GMAC_PHY_SUPPORTED_1000baseX_Half;
        }
    }

  pGMAC->phyStatus.supported &= features;
  pGMAC->phyStatus.advertising &= features;

  PHY_ConfigAneg(pGMAC);

  return HAL_OK;
}

/****************************************************************************
 * Name: GMAC_FlowCtrl
 *
 * Description:
 *   Flow control operation (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int GMAC_FlowCtrl(struct GMAC_HANDLE *pGMAC, int32_t duplex,
                         uint32_t fc, uint32_t pauseTime)
{
  unsigned int flow = 0;

  if (fc & HAL_GMAC_FLOW_RX)
    {
      flow |= GMAC_RX_FLOW_CTRL_RFE;
    }

  putreg32(flow, pGMAC->base + GMAC_MAC_RX_FLOW_CTRL);

  if (fc & HAL_GMAC_FLOW_TX)
    {
      flow = GMAC_Q0_TX_FLOW_CTRL_TFE;
      if (duplex)
        {
          flow |= (pauseTime << GMAC_Q0_TX_FLOW_CTRL_PT_SHIFT);
        }

      putreg32(flow, pGMAC->base + GMAC_MAC_Q0_TX_FLOW_CTRL);
    }
  else
    {
      putreg32(0, pGMAC->base + GMAC_MAC_Q0_TX_FLOW_CTRL);
    }

  return HAL_OK;
}

/****************************************************************************
 * Name: GMAC_DMARXOpMode
 *
 * Description:
 *   Configure DMA RX operation mode (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int GMAC_DMARXOpMode(struct GMAC_HANDLE *pGMAC, int32_t mode,
                            int32_t fifosz)
{
  uint32_t rqs = fifosz / 256 - 1;
  uint32_t mtlRXOP, mtlRXInt;

  mtlRXOP = getreg32(pGMAC->base + GMAC_MTL_RXQ0_OPERATION_MODE);
  if (mode == SF_DMA_MODE)
    {
      mtlRXOP |= MTL_RXQ0_OPERATION_MODE_RSF;
    }
  else
    {
      mtlRXOP &= ~MTL_RXQ0_OPERATION_MODE_RSF;
      mtlRXOP &= MTL_OP_MODE_RTC_MASK;
      if (mode <= 32)
        {
          mtlRXOP |= MTL_OP_MODE_RTC_32;
        }
      else if (mode <= 64)
        {
          mtlRXOP |= MTL_OP_MODE_RTC_64;
        }
      else if (mode <= 96)
        {
          mtlRXOP |= MTL_OP_MODE_RTC_96;
        }
      else
        {
          mtlRXOP |= MTL_OP_MODE_RTC_128;
        }
    }

  mtlRXOP &= ~MTL_OP_MODE_RQS_MASK;
  mtlRXOP |= rqs << MTL_OP_MODE_RQS_SHIFT;

  /* Enable flow control only if each channel gets 4 KiB or more FIFO */

  if (fifosz >= 4096)
    {
      uint32_t rfd, rfa;

      mtlRXOP |= MTL_RXQ0_OPERATION_MODE_EHFC;

      /* Set Threshold for Activating Flow Control to min 2 frames,
       * i.e. 1500 * 2 = 3000 bytes.  Set Threshold for Deactivating
       * Flow Control to min 1 frame, i.e. 1500 bytes.
       */

      switch (fifosz)
        {
        case 4096:
          rfd = 0x03; /* Full-2.5K */
          rfa = 0x01; /* Full-1.5K */
          break;
        case 8192:
          rfd = 0x06; /* Full-4K */
          rfa = 0x0a; /* Full-6K */
          break;
        case 16384:
          rfd = 0x06; /* Full-4K */
          rfa = 0x12; /* Full-10K */
          break;
        default:
          rfd = 0x06; /* Full-4K */
          rfa = 0x1e; /* Full-16K */
          break;
        }

      mtlRXOP &= ~MTL_OP_MODE_RFD_MASK;
      mtlRXOP |= rfd << MTL_OP_MODE_RFD_SHIFT;

      mtlRXOP &= ~MTL_OP_MODE_RFA_MASK;
      mtlRXOP |= rfa << MTL_OP_MODE_RFA_SHIFT;
    }

  putreg32(mtlRXOP, pGMAC->base + GMAC_MTL_RXQ0_OPERATION_MODE);

  /* Enable MTL RX overflow */

  mtlRXInt = getreg32(pGMAC->base + GMAC_MTL_Q0_INTERRUPT_CTRL_STAT);
  putreg32(mtlRXInt | MTL_RX_OVERFLOW_INT_EN,
           pGMAC->base + GMAC_MTL_Q0_INTERRUPT_CTRL_STAT);

  return HAL_OK;
}

/****************************************************************************
 * Name: GMAC_DMATXOpMode
 *
 * Description:
 *   Configure DMA TX operation mode (hal_gmac.c verbatim).
 *
 ****************************************************************************/

static int GMAC_DMATXOpMode(struct GMAC_HANDLE *pGMAC, int32_t mode,
                            int32_t fifosz)
{
  uint32_t tqs = fifosz / 256 - 1;
  uint32_t mtlTXOP;

  mtlTXOP = getreg32(pGMAC->base + GMAC_MTL_TXQ0_OPERATION_MODE);

  if (mode == SF_DMA_MODE)
    {
      /* Transmit COE type 2 cannot be done in cut-through mode. */

      mtlTXOP |= MTL_OP_MODE_TSF;
    }
  else
    {
      mtlTXOP &= ~MTL_OP_MODE_TSF;
      mtlTXOP &= MTL_OP_MODE_TTC_MASK;

      /* Set the transmit threshold */

      if (mode <= 32)
        {
          mtlTXOP |= MTL_OP_MODE_TTC_32;
        }
      else if (mode <= 64)
        {
          mtlTXOP |= MTL_OP_MODE_TTC_64;
        }
      else if (mode <= 96)
        {
          mtlTXOP |= MTL_OP_MODE_TTC_96;
        }
      else if (mode <= 128)
        {
          mtlTXOP |= MTL_OP_MODE_TTC_128;
        }
      else if (mode <= 192)
        {
          mtlTXOP |= MTL_OP_MODE_TTC_192;
        }
      else if (mode <= 256)
        {
          mtlTXOP |= MTL_OP_MODE_TTC_256;
        }
      else if (mode <= 384)
        {
          mtlTXOP |= MTL_OP_MODE_TTC_384;
        }
      else
        {
          mtlTXOP |= MTL_OP_MODE_TTC_512;
        }
    }

  /* For an IP with EQOS_NUM_TXQ == 1, the fields TXQEN and TQS are RO
   * with reset values: TXQEN on, TQS == DWC_EQOS_TXFIFO_SIZE.  For an
   * IP with EQOS_NUM_TXQ > 1, the fields TXQEN and TQS are R/W with
   * reset values: TXQEN off, TQS 256 bytes.  TXQEN must be written for
   * multi-channel operation and TQS must reflect the available fifo
   * size per queue.
   */

  mtlTXOP &= ~MTL_OP_MODE_TXQEN_MASK;
  mtlTXOP |= MTL_OP_MODE_TXQEN;

  mtlTXOP &= ~MTL_OP_MODE_TQS_MASK;
  mtlTXOP |= tqs << MTL_OP_MODE_TQS_SHIFT;

  putreg32(mtlTXOP, pGMAC->base + GMAC_MTL_TXQ0_OPERATION_MODE);

  return HAL_OK;
}

/****************************************************************************
 * Public Functions - RK3506 GRF glue (hal_gmac_rk3506.c verbatim)
 ****************************************************************************/

/****************************************************************************
 * Name: HAL_GMAC_SetToRMII
 *
 * Description:
 *   Set RMII Mode (hal_gmac_rk3506.c verbatim; MAC0 only).
 *
 ****************************************************************************/

void HAL_GMAC_SetToRMII(struct GMAC_HANDLE *pGMAC)
{
  /* Only GMAC0 is wired on this board */

  putreg32(RK3506_MAC_CLK_RMII_MODE,
           RK3506_GRF_ADDR + RK3506_GRF_SOC_CON8);
}

/****************************************************************************
 * Name: HAL_GMAC_SetExtclkSrc
 *
 * Description:
 *   Set external clock source select (hal_gmac_rk3506.c verbatim;
 *   MAC0 only): extClk=0 selects clk_mac (CRU) as the clock of mac,
 *   extClk=1 selects the external phy clock.
 *
 ****************************************************************************/

void HAL_GMAC_SetExtclkSrc(struct GMAC_HANDLE *pGMAC, bool extClk)
{
  bool enable = true;
  uint32_t value;

  value = extClk ? RK3506_MAC_CLK_SELET_IO :
                   RK3506_MAC_CLK_SELET_CRU;
  value |= enable ? RK3506_MAC_CLK_RMII_NOGATE :
                    RK3506_MAC_CLK_RMII_GATE;

  putreg32(value, RK3506_GRF_ADDR + RK3506_GRF_SOC_CON8);
}

/****************************************************************************
 * Name: HAL_GMAC_SetRMIISpeed
 *
 * Description:
 *   Set RMII speed (hal_gmac_rk3506.c verbatim; MAC0 only).
 *
 ****************************************************************************/

void HAL_GMAC_SetRMIISpeed(struct GMAC_HANDLE *pGMAC, int32_t speed)
{
  uint32_t value;

  switch (speed)
    {
    case 10:
      value = RK3506_MAC_CLK_RMII_DIV20;
      break;
    case 100:
      value = RK3506_MAC_CLK_RMII_DIV2;
      break;
    default:
      return;
    }

  putreg32(value, RK3506_GRF_ADDR + RK3506_GRF_SOC_CON8);
}

/****************************************************************************
 * Public Functions - MDIO (hal_gmac.c verbatim)
 ****************************************************************************/

/****************************************************************************
 * Name: HAL_GMAC_MDIORead
 ****************************************************************************/

int32_t HAL_GMAC_MDIORead(struct GMAC_HANDLE *pGMAC, int32_t mdioAddr,
                          int32_t mdioReg)
{
  int status;
  int32_t val;

  status = Mdio_WaitIdle(pGMAC);
  if (status)
    {
      return status;
    }

  val = getreg32(pGMAC->base + GMAC_MAC_MDIO_ADDRESS);
  val &= GMAC_MDIO_ADDRESS_SKAP |
         GMAC_MDIO_ADDRESS_C45E;
  val |= (mdioAddr << GMAC_MDIO_ADDRESS_PA_SHIFT) |
         (mdioReg << GMAC_MDIO_ADDRESS_RDA_SHIFT) |
         (pGMAC->clkCSR <<
          GMAC_MDIO_ADDRESS_CR_SHIFT) |
         GMAC_MDIO_ADDRESS_GOC_READ |
         GMAC_MDIO_ADDRESS_GB;
  putreg32(val, pGMAC->base + GMAC_MAC_MDIO_ADDRESS);

  status = Mdio_WaitIdle(pGMAC);
  if (status)
    {
      return status;
    }

  val = getreg32(pGMAC->base + GMAC_MAC_MDIO_DATA);
  val &= GMAC_MDIO_DATA_GD_MASK;

  return val;
}

/****************************************************************************
 * Name: HAL_GMAC_MDIOWrite
 ****************************************************************************/

int HAL_GMAC_MDIOWrite(struct GMAC_HANDLE *pGMAC, int32_t mdioAddr,
                       int32_t mdioReg, uint16_t mdioVal)
{
  int status;
  uint32_t val;

  status = Mdio_WaitIdle(pGMAC);
  if (status)
    {
      return status;
    }

  putreg32(mdioVal, pGMAC->base + GMAC_MAC_MDIO_DATA);
  val = getreg32(pGMAC->base + GMAC_MAC_MDIO_ADDRESS);
  val &= GMAC_MDIO_ADDRESS_SKAP |
         GMAC_MDIO_ADDRESS_C45E;
  val |= (mdioAddr << GMAC_MDIO_ADDRESS_PA_SHIFT) |
         (mdioReg << GMAC_MDIO_ADDRESS_RDA_SHIFT) |
         (pGMAC->clkCSR <<
          GMAC_MDIO_ADDRESS_CR_SHIFT) |
         GMAC_MDIO_ADDRESS_GOC_WRITE |
         GMAC_MDIO_ADDRESS_GB;
  putreg32(val, pGMAC->base + GMAC_MAC_MDIO_ADDRESS);

  status = Mdio_WaitIdle(pGMAC);
  if (status)
    {
      return status;
    }

  return HAL_OK;
}

/****************************************************************************
 * Public Functions - PHY management (hal_gmac.c verbatim)
 ****************************************************************************/

/****************************************************************************
 * Name: HAL_GMAC_PHYInit
 *
 * Description:
 *   Init PHY by config to connect PHY (hal_gmac.c verbatim).
 *
 ****************************************************************************/

int HAL_GMAC_PHYInit(struct GMAC_HANDLE *pGMAC,
                     struct GMAC_PHY_Config *config)
{
  int32_t i, found = 0, timeout = 500;
  int status;
  int32_t reg;

  if (!config->features)
    {
      config->features = HAL_GMAC_PHY_GBIT_FEATURES |
                         HAL_GMAC_PHY_SUPPORTED_MII |
                         HAL_GMAC_PHY_SUPPORTED_AUI |
                         HAL_GMAC_PHY_SUPPORTED_FIBRE |
                         HAL_GMAC_PHY_SUPPORTED_BNC;
    }

  pGMAC->phyStatus.advertising = pGMAC->phyStatus.supported =
    config->features;

  /* softreset */

  if (HAL_GMAC_MDIOWrite(pGMAC, config->phyAddress, MII_BMCR,
                         BMCR_RESET) < 0)
    {
      return HAL_ERROR;
    }

  reg = HAL_GMAC_MDIORead(pGMAC, config->phyAddress, MII_BMCR);
  while ((reg & BMCR_RESET) && timeout--)
    {
      reg = HAL_GMAC_MDIORead(pGMAC, config->phyAddress, MII_BMCR);
      if (reg < 0)
        {
          return HAL_ERROR;
        }

      HAL_DelayUs(1000);
    }

  if (reg & BMCR_RESET)
    {
      return HAL_TIMEOUT;
    }

  HAL_DelayUs(100000);

  if (config->phyAddress < 0)
    {
      for (i = 0; i < 16; i++)
        {
          status = PHY_Connect(pGMAC, config, i);
          if (!status)
            {
              found++;
              break;
            }
        }
    }
  else
    {
      /* Use PHY addr to connect PHY */

      status = PHY_Connect(pGMAC, config, config->phyAddress);
      if (!status)
        {
          found = 1;
        }
    }

  if (!found)
    {
      return HAL_NODEV;
    }

  if (pGMAC->phyOps.init)
    {
      status = pGMAC->phyOps.init(pGMAC);
    }

  return status;
}

/****************************************************************************
 * Name: HAL_GMAC_PHYUpdateLink
 *
 * Description:
 *   Update link status up/down from PHY (hal_gmac.c verbatim).
 *
 ****************************************************************************/

int HAL_GMAC_PHYUpdateLink(struct GMAC_HANDLE *pGMAC)
{
  int32_t reg;

  /* Wait if the link is up, and autonegotiation is in progress
   * (ie - we're capable and it's not done)
   */

  reg = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_BMSR);
  if (reg < 0)
    {
      return reg;
    }

  /* If we already saw the link up, and it hasn't gone down, then
   * we don't need to wait for autoneg again
   */

  if (pGMAC->phyStatus.link && reg & BMSR_LSTATUS)
    {
      return HAL_OK;
    }

  reg = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_BMSR);
  if (reg & BMSR_LSTATUS)
    {
      pGMAC->phyStatus.link = 1;
    }
  else
    {
      pGMAC->phyStatus.link = 0;
    }

  return HAL_OK;
}

/****************************************************************************
 * Name: HAL_GMAC_PHYParseLink
 *
 * Description:
 *   Updates the speed and duplex (hal_gmac.c verbatim).
 *
 ****************************************************************************/

int HAL_GMAC_PHYParseLink(struct GMAC_HANDLE *pGMAC)
{
  int32_t reg = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_BMSR);

  /* We're using autonegotiation */

  if (pGMAC->phyStatus.neg == PHY_AUTONEG_ENABLE)
    {
      uint32_t lpa = 0, estatus = 0;
      int32_t gblpa = 0;

      /* Check for gigabit capability */

      if (pGMAC->phyStatus.supported &
          (HAL_GMAC_PHY_SUPPORTED_1000baseT_Full |
           HAL_GMAC_PHY_SUPPORTED_1000baseT_Half))
        {
          /* We want a list of states supported by both PHYs in the
           * link
           */

          gblpa = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr,
                                    MII_STAT1000);
          if (gblpa < 0)
            {
              gblpa = 0;
            }

          gblpa &= HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr,
                                     MII_CTRL1000) << 2;
        }

      /* Set the baseline so we only have to set them if they're
       * different
       */

      pGMAC->phyStatus.speed = PHY_SPEED_10M;
      pGMAC->phyStatus.duplex = PHY_DUPLEX_HALF;

      /* Check the gigabit fields */

      if (gblpa & (PHY_1000BTSR_1000FD | PHY_1000BTSR_1000HD))
        {
          pGMAC->phyStatus.speed = PHY_SPEED_1000M;

          if (gblpa & PHY_1000BTSR_1000FD)
            {
              pGMAC->phyStatus.duplex = PHY_DUPLEX_FULL;
            }

          /* We're done! */

          return HAL_OK;
        }

      lpa = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr,
                              MII_ADVERTISE);
      lpa &= HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr, MII_LPA);

      if (lpa & (LPA_100FULL | LPA_100HALF))
        {
          pGMAC->phyStatus.speed = PHY_SPEED_100M;

          if (lpa & LPA_100FULL)
            {
              pGMAC->phyStatus.duplex = PHY_DUPLEX_FULL;
            }
        }
      else if (lpa & LPA_10FULL)
        {
          pGMAC->phyStatus.duplex = PHY_DUPLEX_FULL;
        }

      /* Extended status may indicate that the PHY supports
       * 1000BASE-T/X even though the 1000BASE-T registers are missing.
       */

      if ((reg & BMSR_ESTATEN) && !(reg & BMSR_ERCAP))
        {
          estatus = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr,
                                      MII_ESTATUS);
        }

      if (estatus & (ESTATUS_1000_XFULL | ESTATUS_1000_XHALF |
                     ESTATUS_1000_TFULL | ESTATUS_1000_THALF))
        {
          pGMAC->phyStatus.speed = PHY_SPEED_1000M;
          if (estatus & (ESTATUS_1000_XFULL | ESTATUS_1000_TFULL))
            {
              pGMAC->phyStatus.duplex = PHY_DUPLEX_FULL;
            }
        }

      if (pGMAC->phyStatus.duplex == PHY_DUPLEX_FULL)
        {
          pGMAC->phyStatus.pause = lpa & LPA_PAUSE_CAP ? 1 : 0;
        }
    }
  else
    {
      uint32_t bmcr = HAL_GMAC_MDIORead(pGMAC, pGMAC->phyStatus.addr,
                                        MII_BMCR);

      pGMAC->phyStatus.speed = PHY_SPEED_10M;
      pGMAC->phyStatus.duplex = PHY_DUPLEX_HALF;

      if (bmcr & BMCR_FULLDPLX)
        {
          pGMAC->phyStatus.duplex = PHY_DUPLEX_FULL;
        }

      if (bmcr & BMCR_SPEED1000)
        {
          pGMAC->phyStatus.speed = PHY_SPEED_1000M;
        }
      else if (bmcr & BMCR_SPEED100)
        {
          pGMAC->phyStatus.speed = PHY_SPEED_100M;
        }

      pGMAC->phyStatus.pause = 0;
    }

  return HAL_OK;
}

/****************************************************************************
 * Name: HAL_GMAC_PHYStartup
 *
 * Description:
 *   Start the PHY after configuration (hal_gmac.c verbatim).
 *
 ****************************************************************************/

int HAL_GMAC_PHYStartup(struct GMAC_HANDLE *pGMAC)
{
  int status;

  if (pGMAC->phyStatus.maxSpeed)
    {
      status = PHY_Setsupported(pGMAC, pGMAC->phyStatus.maxSpeed);
      if (status)
        {
          return status;
        }
    }

  status = PHY_Config(pGMAC);
  if (status)
    {
      return status;
    }

  if (pGMAC->phyOps.startup)
    {
      status = pGMAC->phyOps.startup(pGMAC);
      if (status)
        {
          return status;
        }
    }

  return HAL_OK;
}

/****************************************************************************
 * Public Functions - IRQ (hal_gmac.c verbatim)
 ****************************************************************************/

/****************************************************************************
 * Name: HAL_GMAC_IRQHandler
 *
 * Description:
 *   Handle GMAC interrupt for transfer (hal_gmac.c verbatim).
 *
 *   Returns the IRQ status; the caller performs the actual RX/TX work
 *   and (NuttX-specific) must also issue the dcache invalidate for the
 *   RX buffer before use.
 *
 ****************************************************************************/

enum eGMAC_IRQ_Status HAL_GMAC_IRQHandler(struct GMAC_HANDLE *pGMAC)
{
  enum eGMAC_IRQ_Status status = DMA_UNKNOWN;
  uint32_t intrStatus;

  intrStatus = getreg32(pGMAC->base + GMAC_DMA_CH0_STATUS);

  /* ABNORMAL interrupts */

  if (intrStatus & DMA_CHAN_STATUS_AIS)
    {
      if (intrStatus & DMA_CHAN_STATUS_RBU)
        {
          pGMAC->extraStatus.rxBufUnavIRQ++;
          status = DMA_RX_ERROR;
        }
      if (intrStatus & DMA_CHAN_STATUS_RPS)
        {
          pGMAC->extraStatus.rxProcessStoppedIRQ++;
          status = DMA_RX_ERROR;
        }
      if (intrStatus & DMA_CHAN_STATUS_RWT)
        {
          pGMAC->extraStatus.rxWatchdogIRQ++;
          status = DMA_RX_ERROR;
        }
      if (intrStatus & DMA_CHAN_STATUS_ETI)
        {
          pGMAC->extraStatus.txEarlyIRQ++;
          status = DMA_TX_ERROR;
        }
      if (intrStatus & DMA_CHAN_STATUS_TPS)
        {
          pGMAC->extraStatus.txProcessStoppedIRQ++;
          status = DMA_TX_ERROR;
        }
      if (intrStatus & DMA_CHAN_STATUS_FBE)
        {
          pGMAC->extraStatus.fatalBusErrorIRQ++;
          status = DMA_TX_ERROR;
        }
    }

  /* TX/RX NORMAL interrupts */

  if (intrStatus & DMA_CHAN_STATUS_NIS)
    {
      pGMAC->extraStatus.normalIRQN++;
      if (intrStatus & DMA_CHAN_STATUS_RI)
        {
          uint32_t value;

          value = getreg32(pGMAC->base + GMAC_DMA_CH0_INTERRUPT_ENABLE);
          if (value & DMA_CHAN_INTR_ENA_RIE)
            {
              pGMAC->extraStatus.rxNormalIRQN++;
              status |= DMA_HANLE_RX;
            }
        }
      if (intrStatus & DMA_CHAN_STATUS_TI)
        {
          pGMAC->extraStatus.txNormallIRQN++;
          status |= DMA_HANLE_TX;
        }
      if (intrStatus & DMA_CHAN_STATUS_ERI)
        {
          pGMAC->extraStatus.rxEarlyIRQ++;
        }
    }

  /* Clear the interrupt by writing a logic 1 to the chanX interrupt
   * status [21-0] expect reserved bits [5-3]
   */

  putreg32(intrStatus & 0x3fffc7,
           pGMAC->base + GMAC_DMA_CH0_STATUS);

  return status;
}

/****************************************************************************
 * Public Functions - Link / start / stop (hal_gmac.c verbatim)
 ****************************************************************************/

/****************************************************************************
 * Name: HAL_GMAC_AdjustLink
 *
 * Description:
 *   Adjust timing register by link status (hal_gmac.c verbatim).
 *
 ****************************************************************************/

int HAL_GMAC_AdjustLink(struct GMAC_HANDLE *pGMAC, int32_t txDelay,
                        int32_t rxDelay)
{
  uint32_t ctrl;

  ctrl = getreg32(pGMAC->base + GMAC_MAC_CONFIGURATION);
  ctrl |= GMAC_CORE_INIT;

  /* Flow Control operation */

  if (pGMAC->phyStatus.pause)
    {
      GMAC_FlowCtrl(pGMAC, pGMAC->phyStatus.duplex, HAL_GMAC_FLOW_AUTO,
                    HAL_PAUSE_TIME);
    }

  if (!pGMAC->phyStatus.duplex)
    {
      ctrl &= ~pGMAC->link.duplex;
    }
  else
    {
      ctrl |= pGMAC->link.duplex;
    }

  ctrl &= ~pGMAC->link.speedMask;
  switch (pGMAC->phyStatus.speed)
    {
    case PHY_SPEED_1000M:
      ctrl |= pGMAC->link.speed1000;
      break;
    case PHY_SPEED_100M:
      ctrl |= pGMAC->link.speed100;
      break;
    case PHY_SPEED_10M:
      ctrl |= pGMAC->link.speed10;
      break;
    default:
      return HAL_ERROR;
    }

  switch (pGMAC->phyStatus.interface)
    {
    case PHY_INTERFACE_MODE_RMII:
      HAL_GMAC_SetToRMII(pGMAC);
      HAL_GMAC_SetRMIISpeed(pGMAC, pGMAC->phyStatus.speed);
      break;
    case PHY_INTERFACE_MODE_RGMII:
      /* RGMII is not wired on this board; kept for API parity */
      return HAL_ERROR;
    default:
      return HAL_ERROR;
    }

  ctrl |= GMAC_CONFIG_TE;
  putreg32(ctrl, pGMAC->base + GMAC_MAC_CONFIGURATION);

  return HAL_OK;
}

/****************************************************************************
 * Name: HAL_GMAC_Start
 *
 * Description:
 *   Start the GMAC work after configuration (hal_gmac.c verbatim).
 *
 *   The DMA interrupt-enable write (DMA_CHAN_INTR_DEFAULT_MASK) is
 *   verbatim; the GIC line enable is the NuttX caller's job.
 *
 ****************************************************************************/

int HAL_GMAC_Start(struct GMAC_HANDLE *pGMAC, uint8_t *addr)
{
  uint32_t mmc_mode = MMC_CNTRL_RESET_ON_READ | MMC_CNTRL_COUNTER_RESET |
                      MMC_CNTRL_PRESET | MMC_CNTRL_FULL_HALF_PRESET;
  uint32_t mode = SF_DMA_MODE;
  uint32_t value;
  int32_t limit = 10;
  uint32_t hwCap;
  int32_t rxFifosz;
  int32_t txFifosz;

  /* GMAC Software Reset */

  value = getreg32(pGMAC->base + GMAC_DMA_MODE);
  putreg32(value | DMA_MODE_SWR, pGMAC->base + GMAC_DMA_MODE);

  /* Wait for software Reset */

  while (limit--)
    {
      if (!(getreg32(pGMAC->base + GMAC_DMA_MODE) & DMA_MODE_SWR))
        {
          break;
        }

      HAL_DelayMs(10);
    }

  if (limit <= 0)
    {
      return HAL_TIMEOUT;
    }

  HAL_DelayMs(100);

  /* DMA init */

  putreg32(DMA_SYSBUS_MODE_BLEN16 | DMA_SYSBUS_MODE_BLEN8 |
           DMA_SYSBUS_MODE_BLEN4,
           pGMAC->base + GMAC_DMA_SYSBUS_MODE);

  /* Mask interrupts by writing to CSR7 (see function header) */

  putreg32(DMA_CHAN_INTR_DEFAULT_MASK,
           pGMAC->base + GMAC_DMA_CH0_INTERRUPT_ENABLE);

  hwCap = getreg32(pGMAC->base + GMAC_MAC_HW_FEATURE1);

  /* Set the HW DMA mode and the COE */

  txFifosz = 128 << ((hwCap & GMAC_HW_TXFIFOSIZE) >>
                     GMAC_HW_TXFIFOSIZE_SHIFT);
  rxFifosz = 128 << ((hwCap & GMAC_HW_RXFIFOSIZE) >>
                     GMAC_HW_RXFIFOSIZE_SHIFT);

  /* init rx chan.
   *
   * Documented deviation from hal_gmac.c (RBSZ fix): program RBSZ
   * explicitly with the full-field mask to HAL_GMAC_MAX_PACKET_SIZE
   * (1536, the value verified by the M2b.2 loopback; final rxctl =
   * 0x00080c01).  The SDK's (rxFifosz << 1) & 0x3fff truncates to 0
   * for this chip's 8 KiB RX FIFO.  RX buffers use a HAL_GMAC_RXBUF_SIZE
   * (8 KiB) stride >= RBSZ, so the DMA can never overrun a buffer.
   */

  value = getreg32(pGMAC->base + GMAC_DMA_CH0_RX_CONTROL);
  value &= ~DMA_CH0_RX_CONTROL_RBSZ_MASK;
  value |= (HAL_GMAC_MAX_PACKET_SIZE << DMA_CH0_RX_CONTROL_RBSZ_SHIFT) &
           DMA_CH0_RX_CONTROL_RBSZ_MASK;
  value = value | (8 << DMA_CH0_RX_CONTROL_RXPBL_SHIFT);
  putreg32(value, pGMAC->base + GMAC_DMA_CH0_RX_CONTROL);
  putreg32((uintptr_t)pGMAC->rxDescs,
           pGMAC->base + GMAC_DMA_CH0_RXDESC_LIST_ADDR);
  putreg32((uintptr_t)(pGMAC->rxDescs + pGMAC->rxSize),
           pGMAC->base + GMAC_DMA_CH0_RXDESC_TAIL_PTR);

  /* init tx chan */

  value = getreg32(pGMAC->base + GMAC_DMA_CH0_TX_CONTROL);
  value = value | (8 << DMA_CH0_TX_CONTROL_TXPBL_SHIFT);
  value |= DMA_CH0_TX_CONTROL_OSF;
  putreg32(value, pGMAC->base + GMAC_DMA_CH0_TX_CONTROL);

  putreg32((uintptr_t)pGMAC->txDescs,
           pGMAC->base + GMAC_DMA_CH0_TXDESC_LIST_ADDR);
  putreg32((uintptr_t)pGMAC->txDescs,
           pGMAC->base + GMAC_DMA_CH0_TXDESC_TAIL_PTR);

  HAL_GMAC_WriteHWAddr(pGMAC, addr);

  /* Pass all multicast (PACKET_FILTER.PM, Linux dwmac4.h
   * GMAC_PACKET_FILTER_PM): the NuttX driver has no multicast hash
   * bookkeeping, so multicast (IPv6 NDP etc.) must not be dropped.
   */

  value = getreg32(pGMAC->base + GMAC_MAC_PACKET_FILTER);
  putreg32(value | GMAC_PACKET_FILTER_PM,
           pGMAC->base + GMAC_MAC_PACKET_FILTER);

  /* core init */

  value = getreg32(pGMAC->base + GMAC_MAC_CONFIGURATION);
  value |= GMAC_CORE_INIT;
  putreg32(value, pGMAC->base + GMAC_MAC_CONFIGURATION);

  /* enable ipc */

  value = getreg32(pGMAC->base + GMAC_MAC_CONFIGURATION);
  value |= GMAC_CONFIG_IPC;
  putreg32(value, pGMAC->base + GMAC_MAC_CONFIGURATION);

  /* Enable the GMAC Rx/Tx */

  value = getreg32(pGMAC->base + GMAC_MAC_CONFIGURATION);
  value |= GMAC_CONFIG_TE | GMAC_CONFIG_RE;
  putreg32(value, pGMAC->base + GMAC_MAC_CONFIGURATION);

  GMAC_DMARXOpMode(pGMAC, mode, rxFifosz);
  GMAC_DMATXOpMode(pGMAC, mode, txFifosz);

  putreg32(MMC_DEFAULT_MASK,
           pGMAC->base + GMAC_MMC_RX_INTERRUPT_MASK);
  putreg32(MMC_DEFAULT_MASK,
           pGMAC->base + GMAC_MMC_TX_INTERRUPT_MASK);
  putreg32(MMC_DEFAULT_MASK,
           pGMAC->base + GMAC_MMC_IPC_RX_INTERRUPT_MASK);
  value = getreg32(pGMAC->base + GMAC_MMC_CONTROL);
  value |= (mmc_mode & 0x3F);
  putreg32(value, pGMAC->base + GMAC_MMC_CONTROL);

  /* Set TX and RX rings length */

  putreg32(pGMAC->txSize - 1,
           pGMAC->base + GMAC_DMA_CH0_TXDESC_RING_LEN);
  putreg32(pGMAC->rxSize - 1,
           pGMAC->base + GMAC_DMA_CH0_RXDESC_RING_LEN);

  HAL_GMAC_EnableDmaIRQ(pGMAC);

  value = getreg32(pGMAC->base + GMAC_DMA_CH0_TX_CONTROL);
  value |= DMA_CH0_TX_CONTROL_ST;
  putreg32(value, pGMAC->base + GMAC_DMA_CH0_TX_CONTROL);

  value = getreg32(pGMAC->base + GMAC_DMA_CH0_RX_CONTROL);
  value |= DMA_CH0_RX_CONTROL_SR;
  putreg32(value, pGMAC->base + GMAC_DMA_CH0_RX_CONTROL);

  syslog(LOG_INFO, "GMAC HAL Start: rxctl=0x%08lx txctl=0x%08lx"
         " (rbsz=%u bytes)\n",
         (unsigned long)getreg32(pGMAC->base + GMAC_DMA_CH0_RX_CONTROL),
         (unsigned long)getreg32(pGMAC->base + GMAC_DMA_CH0_TX_CONTROL),
         ((unsigned)((getreg32(pGMAC->base + GMAC_DMA_CH0_RX_CONTROL) &
                     0xfffe) >> 1)));

  return HAL_OK;
}

/****************************************************************************
 * Name: HAL_GMAC_Stop
 *
 * Description:
 *   Stop GMAC work (hal_gmac.c verbatim).
 *
 ****************************************************************************/

int HAL_GMAC_Stop(struct GMAC_HANDLE *pGMAC)
{
  struct GMAC_Desc *desc;
  uint32_t value;
  uint32_t i = 0;

  value = getreg32(pGMAC->base + GMAC_DMA_CH0_TX_CONTROL);
  value &= ~DMA_CH0_TX_CONTROL_ST;
  putreg32(value, pGMAC->base + GMAC_DMA_CH0_TX_CONTROL);

  value = getreg32(pGMAC->base + GMAC_DMA_CH0_RX_CONTROL);
  value &= ~DMA_CH0_RX_CONTROL_SR;
  putreg32(value, pGMAC->base + GMAC_DMA_CH0_RX_CONTROL);

  value = getreg32(pGMAC->base + GMAC_MAC_CONFIGURATION);
  value &= ~(GMAC_CONFIG_TE | GMAC_CONFIG_RE);
  putreg32(value, pGMAC->base + GMAC_MAC_CONFIGURATION);

  for (i = 0; i < pGMAC->txSize; i++)
    {
      desc = pGMAC->txDescs + i;
      desc->des0 = 0;
      desc->des1 = 0;
      desc->des2 = 0;
      desc->des3 = 0;
    }

  for (i = 0; i < pGMAC->rxSize; i++)
    {
      desc = pGMAC->rxDescs + i;
      desc->des0 = 0;
      desc->des1 = 0;
      desc->des2 = 0;
      desc->des3 = 0;
    }

  return HAL_OK;
}

/****************************************************************************
 * Public Functions - descriptors / buffers (hal_gmac.c verbatim)
 ****************************************************************************/

/****************************************************************************
 * Name: HAL_GMAC_EnableDmaIRQ / HAL_GMAC_DisableDmaIRQ
 ****************************************************************************/

void HAL_GMAC_EnableDmaIRQ(struct GMAC_HANDLE *pGMAC)
{
  putreg32(DMA_CHAN_INTR_DEFAULT_MASK,
           pGMAC->base + GMAC_DMA_CH0_INTERRUPT_ENABLE);
}

void HAL_GMAC_DisableDmaIRQ(struct GMAC_HANDLE *pGMAC)
{
  putreg32(0, pGMAC->base + GMAC_DMA_CH0_INTERRUPT_ENABLE);
}

/****************************************************************************
 * Name: HAL_GMAC_DMATxDescInit
 *
 * Description:
 *   Initializes the DMA Tx descriptors in chain mode (hal_gmac.c
 *   verbatim).
 *
 ****************************************************************************/

int HAL_GMAC_DMATxDescInit(struct GMAC_HANDLE *pGMAC,
                           struct GMAC_Desc *txDescs,
                           uint8_t *txBuff, uint32_t count)
{
  struct GMAC_Desc *desc;
  uint32_t i = 0;

  pGMAC->txDescIdx = 0;
  pGMAC->txDescs = txDescs;
  pGMAC->txBuf = txBuff;
  pGMAC->txSize = count;

  /* Fill each DMATxDesc descriptor with the right values */

  for (i = 0; i < count; i++)
    {
      desc = txDescs + i;
      desc->des0 = 0;
      desc->des1 = 0;
      desc->des2 = 0;
      desc->des3 = 0;
    }

  return HAL_OK;
}

/****************************************************************************
 * Name: HAL_GMAC_DMARxDescInit
 *
 * Description:
 *   Initializes the DMA Rx descriptors in chain mode (hal_gmac.c
 *   verbatim; buffer stride uses HAL_GMAC_RXBUF_SIZE = the RBSZ the
 *   HAL_GMAC_Start programs, see the header note).
 *
 ****************************************************************************/

int HAL_GMAC_DMARxDescInit(struct GMAC_HANDLE *pGMAC,
                           struct GMAC_Desc *rxDescs,
                           uint8_t *rxBuff, uint32_t count)
{
  struct GMAC_Desc *desc;
  uint32_t i = 0;

  pGMAC->rxDescIdx = 0;
  pGMAC->rxDescs = rxDescs;
  pGMAC->rxBuf = rxBuff;
  pGMAC->rxSize = count;

  /* Fill each DMARxDesc descriptor with the right values */

  for (i = 0; i < count; i++)
    {
      desc = rxDescs + i;
      desc->des0 = (uintptr_t)(rxBuff + i * HAL_GMAC_RXBUF_SIZE);
      desc->des1 = 0;
      desc->des2 = 0;
      desc->des3 = GMAC_DESC3_OWN | GMAC_DESC3_BUF1V | GMAC_DESC3_IOC;
    }

  return HAL_OK;
}

/****************************************************************************
 * Name: HAL_GMAC_GetTXIndex / HAL_GMAC_GetRXIndex
 ****************************************************************************/

uint32_t HAL_GMAC_GetTXIndex(struct GMAC_HANDLE *pGMAC)
{
  return pGMAC->txDescIdx;
}

uint32_t HAL_GMAC_GetRXIndex(struct GMAC_HANDLE *pGMAC)
{
  return pGMAC->txDescIdx;   /* [sic] verbatim from hal_gmac.c */
}

/****************************************************************************
 * Name: HAL_GMAC_GetTXBuffer / HAL_GMAC_GetRXBuffer
 ****************************************************************************/

uint8_t *HAL_GMAC_GetTXBuffer(struct GMAC_HANDLE *pGMAC)
{
  return pGMAC->txBuf + pGMAC->txDescIdx * HAL_GMAC_MAX_PACKET_SIZE;
}

uint8_t *HAL_GMAC_GetRXBuffer(struct GMAC_HANDLE *pGMAC)
{
  return pGMAC->rxBuf + pGMAC->rxDescIdx * HAL_GMAC_RXBUF_SIZE;
}

/****************************************************************************
 * Name: HAL_GMAC_Send
 *
 * Description:
 *   Send data (hal_gmac.c verbatim).
 *
 *   NuttX adaptation: the descriptor write is flushed to RAM with
 *   up_clean_dcache() before the tail pointer is updated (the SDK doc
 *   requires the caller to clean the dcached memory before Send; the
 *   caller still cleans the packet buffer itself).
 *
 ****************************************************************************/

int HAL_GMAC_Send(struct GMAC_HANDLE *pGMAC, void *packet,
                  uint32_t length)
{
  struct GMAC_Desc *desc;
  uint32_t entry;

  entry = pGMAC->txDescIdx;
  desc = pGMAC->txDescs + entry;

  /* Deviation from hal_gmac.c (documented): invalidate the descriptor
   * before reading back its status - the DMA clears OWN in DDR when
   * the frame is done, and with the D-cache enabled the CPU would
   * read its stale cached OWN=1 copy forever, returning HAL_BUSY
   * after the first ETH_TXBUFNB frames (measured on hardware: MMC TX
   * counted 4 frames then never moved again).  Safe w.r.t. dirty
   * lines: every CPU write to a descriptor is immediately followed by
   * up_clean_dcache().
   */

  up_invalidate_dcache((uintptr_t)desc, (uintptr_t)desc + 16);

  if (desc->des3 & GMAC_DESC3_OWN)
    {
      return HAL_BUSY;
    }
  else
    {
      if (desc->des3 & DES3_ERROR_SUMMARY)
        {
          pGMAC->extraStatus.txErrors++;
        }
    }

  pGMAC->txDescIdx = GMAC_GET_ENTRY(entry, pGMAC->txSize);

  desc->des0 = (uintptr_t)packet;
  desc->des1 = 0;
  desc->des2 = length;

  /* Make sure that if HW sees the _OWN write below, it will see all
   * the writes to the rest of the descriptor too.
   *
   * Deviation from hal_gmac.c (documented): OR the total packet
   * length into TDES3[14:0] (Linux dwmac4_descs.c
   * dwmac4_rd_prepare_tx_desc: "tdes3 |= tot_pkt_len &
   * TDES3_PACKET_SIZE_MASK", TDES3_PACKET_SIZE_MASK = GENMASK(14,0)).
   * The SDK HAL leaves TDES3[14:0] = 0; the TX DMA then has no frame
   * length and moves nothing (measured on hardware: MMC
   * TX_PACKET_COUNT_GOOD_BAD stays 0 while NuttX hands frames to the
   * driver - the DHCP DISCOVER never reached the wire).
   */

  desc->des3 = GMAC_DESC3_OWN | GMAC_DESC3_FD | GMAC_DESC3_LD |
               GMAC_DESC3_CIC |
               (length & 0x7fffu);

  up_clean_dcache((uintptr_t)desc, (uintptr_t)desc + 16);

  putreg32((uintptr_t)(pGMAC->txDescs + pGMAC->txDescIdx),
           pGMAC->base + GMAC_DMA_CH0_TXDESC_TAIL_PTR);

  pGMAC->extraStatus.txPktN++;
  pGMAC->extraStatus.txBytesN += length;

  return HAL_OK;
}

/****************************************************************************
 * Name: HAL_GMAC_Recv
 *
 * Description:
 *   Receive data (hal_gmac.c verbatim).  The caller must invalidate
 *   the dcached memory of the returned buffer before use.
 *
 ****************************************************************************/

uint8_t *HAL_GMAC_Recv(struct GMAC_HANDLE *pGMAC, int32_t *length)
{
  struct GMAC_Desc *desc;
  uint32_t entry, des3;
  uint8_t *packet;

  *length = 0;
  entry = pGMAC->rxDescIdx;
  desc = pGMAC->rxDescs + entry;

  /* Deviation from hal_gmac.c (documented): the DMA writes the
   * completion status (OWN=0, frame length) into the descriptor in
   * DDR; with the D-cache enabled the CPU would read its stale cached
   * copy (OWN=1 from our CleanRX write) and never see the completed
   * frame at all (measured on hardware: MMC RX counts frames, no
   * frame ever reached the network stack).  Invalidate the descriptor
   * before reading the status.  Safe w.r.t. dirty lines: every CPU
   * write to a descriptor is immediately followed by
   * up_clean_dcache().
   */

  up_invalidate_dcache((uintptr_t)desc, (uintptr_t)desc + 16);

  des3 = desc->des3;
  if (des3 & GMAC_DESC3_OWN)
    {
      return NULL;
    }

  packet = pGMAC->rxBuf + (pGMAC->rxDescIdx * HAL_GMAC_RXBUF_SIZE);
  *length = des3 & 0x7fff;

  if (des3 & DES3_ERROR_SUMMARY)
    {
      *length = 0;
      pGMAC->extraStatus.rxErrors++;

      return NULL;
    }

  /* If frame length is greater than skb buffer size (preallocated
   * during init) then the packet is ignored
   */

  if (*length > HAL_GMAC_MAX_FRAME_SIZE || *length <= ETH_FCS_LEN)
    {
      *length = 0;
      pGMAC->extraStatus.rxErrors++;

      return NULL;
    }

  *length -= ETH_FCS_LEN;

  pGMAC->extraStatus.rxPktN++;
  pGMAC->extraStatus.rxBytesN += *length;

  return packet;
}

/****************************************************************************
 * Name: HAL_GMAC_CleanRX
 *
 * Description:
 *   Clean Rx dirty description (hal_gmac.c verbatim).
 *
 ****************************************************************************/

void HAL_GMAC_CleanRX(struct GMAC_HANDLE *pGMAC)
{
  struct GMAC_Desc *desc;
  uint32_t entry;

  /* Get the pointer on the ith member of the Rx Desc list */

  entry = pGMAC->rxDescIdx;
  desc = pGMAC->rxDescs + entry;

  desc->des0 = (uintptr_t)(pGMAC->rxBuf + (pGMAC->rxDescIdx *
                                           HAL_GMAC_RXBUF_SIZE));
  desc->des1 = 0;
  desc->des2 = 0;
  desc->des3 = GMAC_DESC3_OWN | GMAC_DESC3_BUF1V | GMAC_DESC3_IOC;

  up_clean_dcache((uintptr_t)desc, (uintptr_t)desc + 16);

  pGMAC->rxDescIdx = GMAC_GET_ENTRY(entry, pGMAC->rxSize);

  putreg32((uintptr_t)(pGMAC->rxDescs + pGMAC->rxDescIdx),
           pGMAC->base + GMAC_DMA_CH0_RXDESC_TAIL_PTR);
}

/****************************************************************************
 * Name: HAL_GMAC_WriteHWAddr
 *
 * Description:
 *   Write MAC address to GMAC (hal_gmac.c verbatim).
 *
 ****************************************************************************/

void HAL_GMAC_WriteHWAddr(struct GMAC_HANDLE *pGMAC, uint8_t *enetAddr)
{
  uint32_t val = 0;

  /* Update the GMAC address.  Deviation from hal_gmac.c (documented):
   * OR in AE (bit31) like Linux stmmac_dwmac4_set_mac_addr()
   * (dwmac4_lib.c: "For MAC Addr registers we have to set the Address
   * Enable (AE) bit"); without AE the perfect filter never matches and
   * all unicast traffic to this MAC is silently dropped.
   */

  val = (enetAddr[5] << 8) |
        (enetAddr[4]) |
        GMAC_ADDR_HIGH_AE;
  putreg32(val, pGMAC->base + GMAC_MAC_ADDRESS0_HIGH);

  val = (enetAddr[3] << 24) |
        (enetAddr[2] << 16) |
        (enetAddr[1] << 8) |
        (enetAddr[0]);
  putreg32(val, pGMAC->base + GMAC_MAC_ADDRESS0_LOW);
}

/****************************************************************************
 * Name: HAL_GMAC_Init
 *
 * Description:
 *   Initialize the GMAC peripheral (hal_gmac.c verbatim).
 *
 ****************************************************************************/

int HAL_GMAC_Init(struct GMAC_HANDLE *pGMAC, uint32_t base, uint32_t freq,
                  enum eGMAC_PHY_Interface interface, bool extClk)
{
  pGMAC->base = base;
  pGMAC->txDescIdx = 0;
  pGMAC->rxDescIdx = 0;

  /* Get CR bits depending on hclk value */

  if ((freq >= 20000000) && (freq < 35000000))
    {
      /* CSR Clock Range between 20-35 MHz */

      pGMAC->clkCSR = GMAC_CSR_20_35M;
    }
  else if ((freq >= 35000000) && (freq < 60000000))
    {
      /* CSR Clock Range between 35-60 MHz */

      pGMAC->clkCSR = GMAC_CSR_35_60M;
    }
  else if ((freq >= 60000000) && (freq < 100000000))
    {
      /* CSR Clock Range between 60-100 MHz */

      pGMAC->clkCSR = GMAC_CSR_60_100M;
    }
  else if ((freq >= 100000000) && (freq < 150000000))
    {
      /* CSR Clock Range between 100-150 MHz */

      pGMAC->clkCSR = GMAC_CSR_100_150M;
    }
  else if ((freq >= 150000000) && (freq < 250000000))
    {
      /* CSR Clock Range between 150-250 MHz */

      pGMAC->clkCSR = GMAC_CSR_150_250M;
    }
  else
    {
      /* CSR Clock Range between 250-300 MHz */

      pGMAC->clkCSR = GMAC_CSR_250_300M;
    }

  pGMAC->link.duplex = GMAC_CONFIG_DM;
  pGMAC->link.speed10 = GMAC_CONFIG_PS;
  pGMAC->link.speed100 = GMAC_CONFIG_FES | GMAC_CONFIG_PS;
  pGMAC->link.speed1000 = 0;
  pGMAC->link.speedMask = GMAC_CONFIG_FES | GMAC_CONFIG_PS;

  pGMAC->mac.miiAddrShift = 21;
  pGMAC->mac.miiAddrMask = 0x03e00000;
  pGMAC->mac.miiRegShift = 16;
  pGMAC->mac.miiRegMask = 0x001f0000;
  pGMAC->mac.clkCsrShift = 8;
  pGMAC->mac.clkCsrMask = 0xf00;

  if (interface == PHY_INTERFACE_MODE_RGMII)
    {
      /* RGMII not wired on this board */

      return HAL_ERROR;
    }
  else
    {
      HAL_GMAC_SetToRMII(pGMAC);
    }

  HAL_GMAC_SetExtclkSrc(pGMAC, extClk);

  return HAL_OK;
}

/****************************************************************************
 * Name: HAL_GMAC_DeInit
 *
 * Description:
 *   DeInitialize the GMAC peripheral (hal_gmac.c verbatim: TO-DO).
 *
 ****************************************************************************/

int HAL_GMAC_DeInit(struct GMAC_HANDLE *pGMAC)
{
  /* TO-DO (verbatim) */

  return HAL_OK;
}
