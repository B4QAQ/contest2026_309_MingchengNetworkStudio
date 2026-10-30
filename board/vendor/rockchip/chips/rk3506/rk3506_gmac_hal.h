/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_gmac_hal.h
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Verbatim NuttX port of the Rockchip SDK GMAC HAL:
 *   hal/lib/hal/inc/hal_gmac.h   (types / API)
 *   hal/lib/hal/src/gmac/hal_gmac.c         (core driver, 3022 lines)
 *   hal/lib/hal/src/gmac/hal_gmac_rk3506.c  (RK3506 GRF glue)
 *
 * Function bodies, register sequences and bit fields are kept identical
 * to the SDK sources; only the OS services (tick/delay, cache maintenance)
 * and the descriptor/buffer memory ownership were adapted.  The register
 * base is addressed through fixed GMAC_*_OFFSET defines taken from
 * rk3506.h (struct GMAC_REG) instead of the packed struct pointer.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_GMAC_HAL_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_GMAC_HAL_H

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GMAC descriptions and buffers Definition (hal_gmac.h) */

#define HAL_GMAC_ALIGN(x, a)      (((x) + (a) - 1) & ~((a) - 1))
#define HAL_GMAC_DESCRIPTOR_WORDS 4
#define HAL_GMAC_DESCRIPTOR_SIZE  (HAL_GMAC_DESCRIPTOR_WORDS * 4)
#define HAL_GMAC_BUFFER_ALIGN     64
#define HAL_GMAC_MAX_FRAME_SIZE   1518
#define HAL_GMAC_MAX_PACKET_SIZE  HAL_GMAC_ALIGN(HAL_GMAC_MAX_FRAME_SIZE, \
                                                 HAL_GMAC_BUFFER_ALIGN)

/* GMAC flow ctrl Definition (hal_gmac.h) */

#define HAL_GMAC_FLOW_OFF  0
#define HAL_GMAC_FLOW_RX   1
#define HAL_GMAC_FLOW_TX   2
#define HAL_GMAC_FLOW_AUTO (HAL_GMAC_FLOW_TX | HAL_GMAC_FLOW_RX)

/* GMAC PHY indicates what features are supported (hal_gmac.h) */

#define HAL_GMAC_PHY_SUPPORTED_10baseT_Half      (1 << 0)
#define HAL_GMAC_PHY_SUPPORTED_10baseT_Full      (1 << 1)
#define HAL_GMAC_PHY_SUPPORTED_100baseT_Half     (1 << 2)
#define HAL_GMAC_PHY_SUPPORTED_100baseT_Full     (1 << 3)
#define HAL_GMAC_PHY_SUPPORTED_1000baseT_Half    (1 << 4)
#define HAL_GMAC_PHY_SUPPORTED_1000baseT_Full    (1 << 5)
#define HAL_GMAC_PHY_SUPPORTED_Autoneg           (1 << 6)
#define HAL_GMAC_PHY_SUPPORTED_TP                (1 << 7)
#define HAL_GMAC_PHY_SUPPORTED_AUI               (1 << 8)
#define HAL_GMAC_PHY_SUPPORTED_MII               (1 << 9)
#define HAL_GMAC_PHY_SUPPORTED_FIBRE             (1 << 10)
#define HAL_GMAC_PHY_SUPPORTED_BNC               (1 << 11)
#define HAL_GMAC_PHY_SUPPORTED_Pause             (1 << 13)
#define HAL_GMAC_PHY_SUPPORTED_Asym_Pause        (1 << 14)
#define HAL_GMAC_PHY_SUPPORTED_1000baseX_Half    (1 << 21)
#define HAL_GMAC_PHY_SUPPORTED_1000baseX_Full    (1 << 22)

#define HAL_GMAC_PHY_DEFAULT_FEATURES (HAL_GMAC_PHY_SUPPORTED_Autoneg | \
                                       HAL_GMAC_PHY_SUPPORTED_TP |      \
                                       HAL_GMAC_PHY_SUPPORTED_MII)

#define HAL_GMAC_PHY_10BT_FEATURES (HAL_GMAC_PHY_SUPPORTED_10baseT_Half | \
                                    HAL_GMAC_PHY_SUPPORTED_10baseT_Full)

#define HAL_GMAC_PHY_100BT_FEATURES (HAL_GMAC_PHY_SUPPORTED_100baseT_Half | \
                                     HAL_GMAC_PHY_SUPPORTED_100baseT_Full)

#define HAL_GMAC_PHY_BASIC_FEATURES (HAL_GMAC_PHY_10BT_FEATURES |  \
                                     HAL_GMAC_PHY_100BT_FEATURES | \
                                     HAL_GMAC_PHY_DEFAULT_FEATURES)

#define HAL_GMAC_PHY_GBIT_FEATURES (HAL_GMAC_PHY_BASIC_FEATURES | \
                                    HAL_GMAC_PHY_SUPPORTED_1000baseT_Half | \
                                    HAL_GMAC_PHY_SUPPORTED_1000baseT_Full)

/* HAL_Status equivalent (nuttx has no HAL_Status; use 0/(-errno)) */

#define HAL_OK       0
#define HAL_ERROR    (-1)
#define HAL_TIMEOUT  (-2)
#define HAL_NODEV    (-3)
#define HAL_BUSY     (-4)

/* RX buffer stride.  HAL_GMAC_Start() programs RBSZ = 1536
 * (HAL_GMAC_MAX_PACKET_SIZE, the loopback-verified value; the SDK's
 * 8 KiB-RBSZ expression truncates to 0 on this chip and drops every
 * RX frame).  The DMA never writes more than RBSZ bytes per buffer,
 * so the 8 KiB stride is pure headroom kept so the ring layout is
 * independent of any future RBSZ change.  TX keeps
 * HAL_GMAC_MAX_PACKET_SIZE.
 */

#define HAL_GMAC_RXBUF_SIZE  8192

/* MMC hardware counter register offsets (rk3506.h), used by the
 * netdev bring-up statistics print (link poll, change-detect).
 */

#define GMAC_TX_PACKET_COUNT_GOOD_BAD   0x0718u
#define GMAC_RX_PACKETS_COUNT_GOOD_BAD  0x0780u
#define GMAC_RX_CRC_ERROR_PACKETS       0x0794u
#define GMAC_RX_LENGTH_ERROR_PACKETS    0x07c8u
#define GMAC_RX_FIFO_OVERFLOW_PACKETS   0x07d4u

/****************************************************************************
 * Types
 ****************************************************************************/

/* GMAC PHY Speed (hal_gmac.h) */

enum eGMAC_PHY_SPEED
{
  PHY_SPEED_10M   = 10,
  PHY_SPEED_100M  = 100,
  PHY_SPEED_1000M = 1000,
};

/* GMAC PHY Duplex (hal_gmac.h) */

enum eGMAC_PHY_DUPLEX
{
  PHY_DUPLEX_HALF = 0,
  PHY_DUPLEX_FULL = 1,
};

/* GMAC PHY Auto Negotiation (hal_gmac.h) */

enum eGMAC_PHY_NEGROTETION
{
  PHY_AUTONEG_DISABLE = 0,
  PHY_AUTONEG_ENABLE  = 1,
};

/* GMAC PHY Interface Mode (hal_gmac.h) */

enum eGMAC_PHY_Interface
{
  PHY_INTERFACE_MODE_MII,
  PHY_INTERFACE_MODE_RMII,
  PHY_INTERFACE_MODE_RGMII,
  PHY_INTERFACE_MODE_NONE,
};

/* GMAC DMA IRQ Status (hal_gmac.h) */

enum eGMAC_IRQ_Status
{
  DMA_UNKNOWN  = 0x0,
  DMA_HANLE_RX = 0x1,
  DMA_HANLE_TX = 0x2,
  DMA_TX_ERROR = 0x10,
  DMA_RX_ERROR = 0x20,
};

/* GMAC DMA Descriptors Data Structure (hal_gmac.h) */

struct GMAC_Desc
{
  uint32_t des0;
  uint32_t des1;
  uint32_t des2;
  uint32_t des3;
};

struct GMAC_HANDLE;

/* GMAC PHY OPS Structure (hal_gmac.h) */

struct GMAC_PHY_OPS
{
  int (*init)(struct GMAC_HANDLE *pGMAC);
  int (*config)(struct GMAC_HANDLE *pGMAC);
  int (*startup)(struct GMAC_HANDLE *pGMAC);
  int (*shutdown)(struct GMAC_HANDLE *pGMAC);
  int (*reset)(struct GMAC_HANDLE *pGMAC);
  int (*softreset)(struct GMAC_HANDLE *pGMAC);
};

/* GMAC PHY Config Structure (hal_gmac.h) */

struct GMAC_PHY_Config
{
  enum eGMAC_PHY_Interface interface;
  int16_t phyAddress;
  enum eGMAC_PHY_NEGROTETION neg;
  enum eGMAC_PHY_SPEED speed;
  enum eGMAC_PHY_DUPLEX duplexMode;
  enum eGMAC_PHY_SPEED maxSpeed;
  uint32_t features;
};

/* GMAC PHY Status Structure (hal_gmac.h) */

struct GMAC_PHY_STATUS
{
  enum eGMAC_PHY_Interface interface;
  enum eGMAC_PHY_SPEED speed;
  enum eGMAC_PHY_DUPLEX duplex;
  enum eGMAC_PHY_SPEED maxSpeed;
  enum eGMAC_PHY_NEGROTETION neg;
  int link;
  int oldLink;
  uint32_t features;
  uint32_t advertising;
  uint32_t supported;
  int16_t addr;
  int16_t pause;
  uint32_t phyID;
};

/* GMAC Link Config Structure (hal_gmac.h) */

struct GMAC_Link
{
  uint32_t speedMask;
  uint32_t speed10;
  uint32_t speed100;
  uint32_t speed1000;
  uint32_t duplex;
};

/* GMAC DMA Transfer Status (hal_gmac.h) */

struct GMAC_DMAStats
{
  uint32_t txUndeflowIRQ;
  uint32_t txProcessStoppedIRQ;
  uint32_t txJabberIRQ;
  uint32_t rxOverflowIRQ;
  uint32_t rxBufUnavIRQ;
  uint32_t rxProcessStoppedIRQ;
  uint32_t rxWatchdogIRQ;
  uint32_t txEarlyIRQ;
  uint32_t fatalBusErrorIRQ;
  uint32_t normalIRQN;
  uint32_t rxNormalIRQN;
  uint32_t txNormallIRQN;
  uint32_t rxEarlyIRQ;
  uint32_t thresHold;
  uint32_t txPktN;
  uint32_t rxPktN;
  uint32_t txBytesN;
  uint32_t rxBytesN;
  uint32_t txErrors;
  uint32_t rxErrors;
};

/* GMAC device information (hal_gmac.h) */

struct GMAC_DEVICE_INFO
{
  uint32_t miiAddrShift;
  uint32_t miiAddrMask;
  uint32_t miiRegShift;
  uint32_t miiRegMask;
  uint32_t clkCsrShift;
  uint32_t clkCsrMask;
};

/* GMAC handle (hal_gmac.h; PTP block omitted) */

struct GMAC_HANDLE
{
  uint32_t base;                    /* register base (physical == virtual) */
  uint32_t clkCSR;                  /* clock csr value, div for MDC clock */

  struct GMAC_DEVICE_INFO mac;

  struct GMAC_PHY_OPS phyOps;
  struct GMAC_PHY_Config phyConfig;
  struct GMAC_PHY_STATUS phyStatus;

  struct GMAC_Link link;
  struct GMAC_DMAStats extraStatus;

  struct GMAC_Desc *rxDescs;
  struct GMAC_Desc *txDescs;
  uint8_t *txBuf;
  uint8_t *rxBuf;
  uint32_t txDescIdx;
  uint32_t rxDescIdx;
  uint32_t txSize;
  uint32_t rxSize;
};

/****************************************************************************
 * Public Function Prototypes (hal_gmac.h API, verbatim names)
 ****************************************************************************/

int HAL_GMAC_Init(struct GMAC_HANDLE *pGMAC, uint32_t base, uint32_t freq,
                  enum eGMAC_PHY_Interface interface, bool extClk);
int HAL_GMAC_DeInit(struct GMAC_HANDLE *pGMAC);
int HAL_GMAC_Start(struct GMAC_HANDLE *pGMAC, uint8_t *addr);
int HAL_GMAC_Stop(struct GMAC_HANDLE *pGMAC);
void HAL_GMAC_EnableDmaIRQ(struct GMAC_HANDLE *pGMAC);
void HAL_GMAC_DisableDmaIRQ(struct GMAC_HANDLE *pGMAC);
int HAL_GMAC_DMATxDescInit(struct GMAC_HANDLE *pGMAC,
                           struct GMAC_Desc *txDescs,
                           uint8_t *txBuff, uint32_t txBuffCount);
int HAL_GMAC_DMARxDescInit(struct GMAC_HANDLE *pGMAC,
                           struct GMAC_Desc *rxDescs,
                           uint8_t *rxBuff, uint32_t rxBuffCount);
enum eGMAC_IRQ_Status HAL_GMAC_IRQHandler(struct GMAC_HANDLE *pGMAC);
int HAL_GMAC_AdjustLink(struct GMAC_HANDLE *pGMAC, int32_t txDelay,
                        int32_t rxDelay);
uint32_t HAL_GMAC_GetTXIndex(struct GMAC_HANDLE *pGMAC);
uint32_t HAL_GMAC_GetRXIndex(struct GMAC_HANDLE *pGMAC);
uint8_t *HAL_GMAC_GetTXBuffer(struct GMAC_HANDLE *pGMAC);
uint8_t *HAL_GMAC_GetRXBuffer(struct GMAC_HANDLE *pGMAC);
int HAL_GMAC_Send(struct GMAC_HANDLE *pGMAC, void *packet, uint32_t length);
uint8_t *HAL_GMAC_Recv(struct GMAC_HANDLE *pGMAC, int32_t *length);
void HAL_GMAC_CleanRX(struct GMAC_HANDLE *pGMAC);
void HAL_GMAC_WriteHWAddr(struct GMAC_HANDLE *pGMAC, uint8_t *enetAddr);
int HAL_GMAC_PHYInit(struct GMAC_HANDLE *pGMAC,
                     struct GMAC_PHY_Config *config);
int HAL_GMAC_PHYStartup(struct GMAC_HANDLE *pGMAC);
int HAL_GMAC_PHYUpdateLink(struct GMAC_HANDLE *pGMAC);
int HAL_GMAC_PHYParseLink(struct GMAC_HANDLE *pGMAC);
int32_t HAL_GMAC_MDIORead(struct GMAC_HANDLE *pGMAC, int32_t mdioAddr,
                          int32_t mdioReg);
int HAL_GMAC_MDIOWrite(struct GMAC_HANDLE *pGMAC, int32_t mdioAddr,
                       int32_t mdioReg, uint16_t mdioVal);
void HAL_GMAC_SetToRMII(struct GMAC_HANDLE *pGMAC);
void HAL_GMAC_SetRMIISpeed(struct GMAC_HANDLE *pGMAC, int32_t speed);
void HAL_GMAC_SetExtclkSrc(struct GMAC_HANDLE *pGMAC, bool extClk);

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_GMAC_HAL_H */
