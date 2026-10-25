/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_mmc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506 SD/MMC host controller driver: Synopsys DesignWare MSHC
 * ("rockchip,rk3506-dw-mshc") as a NuttX sdio_dev_s lower half feeding
 * drivers/mmcsd (mmcsd_slotinitialize).
 *
 * Hardware knowledge base (Linux SDK, authoritative):
 *  - u-boot/arch/arm/dts/rk3506.dtsi:858
 *      mmc@ff480000, reg 0x4000, GIC SPI 86, bus-width 4,
 *      max-frequency 150 MHz, fifo-depth 0x100 (256 bytes),
 *      clocks "biu" = HCLK_SDMMC, "ciu" = CCLK_SRC_SDMMC.
 *  - hal/lib/CMSIS/Device/RK3506/Include/rk3506.h
 *      MMC_BASE 0xFF480000, struct MMC_REG register map.
 *  - u-boot/include/dwmmc.h  : register offsets + bit fields (verbatim
 *      below; every offset double-checked against struct MMC_REG).
 *  - u-boot/drivers/mmc/dw_mmc.c : dwmci_send_cmd / dwmci_data_transfer
 *      command issue order, response extraction, FIFO PIO pumping.
 *  - u-boot/drivers/mmc/rockchip_dw_mmc.c : FIFOTH value for this SoC
 *      (MSIZE(6) | RX_WMARK(depth/2 - 1) | TX_WMARK(depth/2)).
 *  - hal/lib/hal/src/hal_sdio.c : HAL_MMC_Init / _Reset / _ResetFifo /
 *      _UpdateClockRegister / _SendCommand / _ReadData / _WriteData.
 *  - kernel-6.1/drivers/clk/rockchip/clk-rk3506.c:582-586
 *      CCLK_SRC_SDMMC = COMPOSITE CLKSEL_CON(49) mux[14:13] div[12:7]
 *      gate CLKGATE_CON(17) bit 6; HCLK_SDMMC = gate bit 7.
 *  - kernel-6.1/drivers/mmc/host/dw_mmc.c
 *      card clock = cclk_in / ((CLKDIV & 0xff) * 2)  (lines 398/1342).
 *
 * Transfer model: polled PIO (no IDMAC, no IRQ).  See the design note in
 * rk3506_mmc_data() for why, and what to change for DMA/IRQ later.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/mmcsd.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/wqueue.h>

#include <debug.h>

#include "rk3506_cru.h"
#include "rk3506_mmc.h"
#include "hardware/rk3506_memorymap.h"

#ifdef CONFIG_RK3506_MMC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base (hardware/rk3506_memorymap.h, dts and CMSIS MMC_BASE all
 * agree: 0xFF480000).
 */

#define RK3506_MMC_BASE           RK3506_SDMMC_ADDR

/* Register offsets (u-boot/include/dwmmc.h:14-52) */

#define MMC_CTRL                  0x000
#define MMC_PWREN                 0x004
#define MMC_CLKDIV                0x008
#define MMC_CLKSRC                0x00c
#define MMC_CLKENA                0x010
#define MMC_TMOUT                 0x014
#define MMC_CTYPE                 0x018
#define MMC_BLKSIZ                0x01c
#define MMC_BYTCNT                0x020
#define MMC_INTMASK               0x024
#define MMC_CMDARG                0x028
#define MMC_CMD                   0x02c
#define MMC_RESP0                 0x030
#define MMC_RESP1                 0x034
#define MMC_RESP2                 0x038
#define MMC_RESP3                 0x03c
#define MMC_MINTSTS               0x040
#define MMC_RINTSTS               0x044
#define MMC_STATUS                0x048
#define MMC_FIFOTH                0x04c
#define MMC_CDETECT               0x050
#define MMC_WRTPRT                0x054
#define MMC_TCBCNT                0x05c
#define MMC_TBBCNT                0x060
#define MMC_DEBNCE                0x064
#define MMC_VERID                 0x06c
#define MMC_BMOD                  0x080
#define MMC_DATA                  0x200

#define MMC_REG(off)              (RK3506_MMC_BASE + (off))

/* CTRL (u-boot dwmmc.h:76-83, HAL MMC_CTRL_*) */

#define CTRL_RESET                (1u << 0)
#define CTRL_FIFO_RESET           (1u << 1)
#define CTRL_DMA_RESET            (1u << 2)
#define CTRL_INT_ENABLE           (1u << 4)
#define CTRL_DMA_ENABLE           (1u << 5)
#define CTRL_RESET_ALL            (CTRL_RESET | CTRL_FIFO_RESET | \
                                   CTRL_DMA_RESET)

/* BMOD (dwmmc.h:130-132) */

#define BMOD_SWR                  (1u << 0)
#define BMOD_FB                   (1u << 1)   /* fixed burst */

/* CLKENA (dwmmc.h:100-101) */

#define CLKENA_CCLK_ENABLE        (1u << 0)
#define CLKENA_CCLK_LOW_POWER     (1u << 16)

/* CTYPE (dwmmc.h:104-106) */

#define CTYPE_WIDTH1              0u
#define CTYPE_WIDTH4              (1u << 0)
#define CTYPE_WIDTH8              (1u << 16)

/* CMD (dwmmc.h:86-97) */

#define CMD_RESP_EXP              (1u << 6)
#define CMD_RESP_LENGTH           (1u << 7)   /* 136-bit response */
#define CMD_CHECK_CRC             (1u << 8)
#define CMD_DATA_EXP              (1u << 9)
#define CMD_RW                    (1u << 10)  /* 1 = write to card */
#define CMD_SEND_STOP             (1u << 12)  /* auto CMD12 after data */
#define CMD_PRV_DAT_WAIT          (1u << 13)
#define CMD_ABORT_STOP            (1u << 14)
#define CMD_SEND_INIT             (1u << 15)
#define CMD_UPD_CLK               (1u << 21)
#define CMD_USE_HOLD              (1u << 29)
#define CMD_START                 (1u << 31)

/* RINTSTS / INTMASK bit map (dwmmc.h:56-74, HAL MMC_INT_STATUS_*) */

#define INT_CD                    (1u << 0)   /* card detect */
#define INT_RE                    (1u << 1)   /* response error */
#define INT_CDONE                 (1u << 2)   /* command done */
#define INT_DTO                   (1u << 3)   /* data transfer over */
#define INT_TXDR                  (1u << 4)   /* TX FIFO ready */
#define INT_RXDR                  (1u << 5)   /* RX FIFO ready */
#define INT_DCRC                  (1u << 6)   /* response CRC error */
#define INT_DCRC_DATA             (1u << 7)   /* data CRC error */
#define INT_RTO                   (1u << 8)   /* response timeout */
#define INT_DRTO                  (1u << 9)   /* data read timeout */
#define INT_HTO                   (1u << 10)  /* data starvation/host TO */
#define INT_FRUN                  (1u << 11)  /* FIFO over/underrun */
#define INT_HLE                   (1u << 12)  /* hardware locked error */
#define INT_SBE                   (1u << 13)  /* start bit error */
#define INT_ACD                   (1u << 14)  /* auto CMD12 done */
#define INT_EBE                   (1u << 15)  /* end bit error */
#define INT_SDIO                  (1u << 24)
#define INT_ALL                   0xffffffffu

#define INT_DATA_ERR              (INT_EBE | INT_SBE | INT_HLE | INT_FRUN | \
                                   INT_DCRC_DATA)
#define INT_DATA_TOUT             (INT_HTO | INT_DRTO)
#define INT_ANY_ERROR             (INT_RE | INT_DCRC | INT_RTO | \
                                   INT_DATA_ERR | INT_DATA_TOUT)

/* STATUS (CMSIS rk3506.h:7475-7494) */

#define STATUS_FIFO_COUNT_SHIFT   17
#define STATUS_FIFO_COUNT_MASK    (0x1fffu << STATUS_FIFO_COUNT_SHIFT)
#define STATUS_FIFO_COUNT(r)      (((r) >> 17) & 0x1fffu)
#define STATUS_DATA_BUSY          (1u << 9)
#define STATUS_MC_BUSY            (1u << 10)

/* CDETECT / WRTPRT: bit 0 is active LOW (0 = card present) */

#define CDETECT_CARD_PRESENT_N    (1u << 0)

/* FIFOTH: MSIZE(6) | RX_WMARK(127) | TX_WMARK(128) for the 256-byte FIFO
 * (u-boot rockchip_dw_mmc.c:419-420 with dts fifo-depth = 0x100).
 */

#define FIFO_DEPTH_BYTES          256
#define FIFO_DEPTH_WORDS          (FIFO_DEPTH_BYTES / 4)
#define FIFOTH_VALUE              ((6u << 28) | (127u << 16) | 128u)

/* Timeouts (millisecond values fed to clock_msleep() based loops) */

#define MMC_CMD_TIMEOUT_MS        1000   /* one command + response       */
#define MMC_DATA_TIMEOUT_MS       5000   /* one data transfer            */
#define MMC_RESET_TIMEOUT_MS      100    /* BMOD/CTRL/FIFO soft reset    */

/* Clock tree: CCLK_SRC_SDMMC mux/divider (clk-rk3506.c:582) */

#define CLKSEL_CON_SDMMC          49
#define SDMMC_SEL_SHIFT           13
#define SDMMC_SEL_MASK            (0x3u << SDMMC_SEL_SHIFT)
#define SDMMC_DIV_SHIFT           7
#define SDMMC_DIV_MASK            (0x3fu << SDMMC_DIV_SHIFT)
#define SDMMC_SEL_24M             0
#define SDMMC_SEL_GPLL            1
#define SDMMC_SEL_V0PLL           2
#define SDMMC_SEL_V1PLL           3

/* Gate bits in CLKGATE_CON(17) (clk-rk3506.c:582-586) */

#define CLKGATE_CON_SDMMC         17
#define HCLK_SDMMC_GATE           (1u << 7)
#define CCLK_SRC_SDMMC_GATE       (1u << 6)

/* Card clock selection */

#define MMC_IDMODE_FREQ           400000u     /* <= 400 kHz, spec max */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_mmc_priv_s
{
  struct sdio_dev_s dev;         /* Must be first: we ARE the lower half */

  /* Pending data-setup recorded by recvsetup()/sendsetup() */

  FAR uint8_t *rxbuffer;
  FAR const uint8_t *txbuffer;
  size_t xfrlen;
  bool pendsetup;
  bool pendwrite;

  /* Last command results */

  uint32_t response[4];
  sdio_eventset_t wkupevent;    /* What happened during sendcmd() */
  sdio_eventset_t waitevents;   /* Set by waitenable() */
  sdio_capset_t caps;
  bool widebus;

  /* Media change plumbing */

  bool present;                 /* Last observed insertion state */
#ifdef CONFIG_SCHED_WORKQUEUE
  struct work_s work;
  worker_t callback;
  FAR void *cbarg;
  sdio_eventset_t cbevents;
#endif
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3506_mmc_priv_s g_mmc0;
static bool g_mmc_registered;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t mmc_getreg(uint32_t off)
{
  return getreg32(MMC_REG(off));
}

static inline void mmc_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, MMC_REG(off));
}

/****************************************************************************
 * Name: rk3506_mmc_wait_bits
 *
 * Description:
 *   Poll a register until (reg & mask) == value, or timeout.
 *
 ****************************************************************************/

static int rk3506_mmc_wait_bits(uint32_t off, uint32_t mask, uint32_t value,
                                unsigned int timeout_ms)
{
  clock_t start = clock_systime_ticks();
  clock_t ticks = MSEC2TICKS(timeout_ms);

  for (; ; )
    {
      if ((mmc_getreg(off) & mask) == value)
        {
          return OK;
        }

      if ((clock_systime_ticks() - start) > ticks)
        {
          return -ETIMEDOUT;
        }
    }
}

/****************************************************************************
 * Name: rk3506_mmc_send_ciu_cmd
 *
 * Description:
 *   Send the DesignWare "inform CIU" pseudo-command: START | UPD_CLK with
 *   no index (HAL_MMC_UpdateClockRegister() uses exactly
 *   WRITE_REG(CMD, HAL_BIT(31) | HAL_BIT(21)) and polls START clear).
 *   Required after every CLKENA/CLKSRC/CLKDIV change for it to take effect.
 *
 ****************************************************************************/

static int rk3506_mmc_send_ciu_cmd(void)
{
  mmc_putreg(MMC_CMD, CMD_START | CMD_UPD_CLK);

  return rk3506_mmc_wait_bits(MMC_CMD, CMD_START, 0, MMC_RESET_TIMEOUT_MS);
}

/****************************************************************************
 * Name: rk3506_mmc_clock_init
 *
 * Description:
 *   Ungate HCLK_SDMMC + CCLK_SRC_SDMMC and program the CCLK_SRC_SDMMC
 *   mux/divider so the controller's cclk_in ("ciu") clock is running.
 *
 *   Per clk-rk3506.c: CCLK_SRC_SDMMC parents are
 *   xin24m(0) / gpll(1) / clk_v0pll(2) / clk_v1pll(3).
 *
 *   CONFIG_RK3506_MMC_CIU_FROM_XIN24M=y selects the 24 MHz oscillator with
 *   divider /1, which is guaranteed-correct with no PLL assumption but
 *   caps the card clock at 12 MHz.  The default (n) uses GPLL with
 *   divider = CONFIG_RK3506_MMC_GPLL_HZ / CONFIG_RK3506_MMC_CIU_HZ.
 *
 ****************************************************************************/

static void rk3506_mmc_clock_init(void)
{
  uint32_t sel;
  uint32_t div;

  /* Ungate both clocks: data bit 0 = enable (SET_TO_DISABLE gates), so
   * rk3506_cru_gate(con, mask, true) writes (mask << 16) | 0.
   */

  rk3506_cru_gate(CLKGATE_CON_SDMMC,
                  HCLK_SDMMC_GATE | CCLK_SRC_SDMMC_GATE, true);

#ifdef CONFIG_RK3506_MMC_CIU_FROM_XIN24M
  sel = SDMMC_SEL_24M;
  div = 1;
#else
  sel = SDMMC_SEL_GPLL;

  div = (CONFIG_RK3506_MMC_GPLL_HZ + (CONFIG_RK3506_MMC_CIU_HZ / 2)) /
        CONFIG_RK3506_MMC_CIU_HZ;

  if (div < 1)
    {
      div = 1;
    }

  if (div > 64)
    {
      div = 64;
    }
#endif

  /* HIWORD_UPDATE: only mux[14:13] and div[12:7] are touched.  The
   * register field holds divider - 1 (u-boot clk_rk3506.c:543).
   */

  putreg32(((SDMMC_SEL_MASK | SDMMC_DIV_MASK) << 16) |
           ((sel << SDMMC_SEL_SHIFT) | ((div - 1) << SDMMC_DIV_SHIFT)),
           RK3506_CRU_CLKSEL_CON(CLKSEL_CON_SDMMC));

  _info("MMC: ciu = %" PRIu32 " Hz (sel %" PRIu32 ", div %" PRIu32 ")\n",
        CONFIG_RK3506_MMC_CIU_HZ, sel, div);
}

/****************************************************************************
 * Name: rk3506_mmc_reset
 *
 * Description:
 *   Controller soft reset.  Mirrors HAL_MMC_Reset():
 *    BMOD.SWR self-clearing first, then CTRL{CONTROLLER,FIFO,DMA}_RESET,
 *    both polled for self-clear (hal_sdio.c:501-534).
 *
 *   NOTE: no CRU reset is touched.  The SDK's cru_rk3506.h and
 *   clk-rk3506.c contain no SRST_*_SDMMC id, so rather than invent one we
 *   rely on the controller's own reset, exactly like the HAL does.
 *
 ****************************************************************************/

static int rk3506_mmc_reset(void)
{
  int ret;

  mmc_putreg(MMC_BMOD, mmc_getreg(MMC_BMOD) | BMOD_SWR);
  ret = rk3506_mmc_wait_bits(MMC_BMOD, BMOD_SWR, 0, MMC_RESET_TIMEOUT_MS);
  if (ret < 0)
    {
      _err("MMC: BMOD software reset timeout\n");
      return ret;
    }

  mmc_putreg(MMC_CTRL, mmc_getreg(MMC_CTRL) | CTRL_RESET_ALL);
  ret = rk3506_mmc_wait_bits(MMC_CTRL, CTRL_RESET_ALL, 0,
                             MMC_RESET_TIMEOUT_MS);
  if (ret < 0)
    {
      _err("MMC: CTRL dma|fifo|ctrl reset timeout\n");
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3506_mmc_reset_fifo
 *
 * Description:
 *   CTRL.FIFO_RESET + poll (HAL_MMC_ResetFifo(), hal_sdio.c:474-494).
 *
 ****************************************************************************/

static int rk3506_mmc_reset_fifo(void)
{
  int ret;

  mmc_putreg(MMC_CTRL, mmc_getreg(MMC_CTRL) | CTRL_FIFO_RESET);
  ret = rk3506_mmc_wait_bits(MMC_CTRL, CTRL_FIFO_RESET, 0,
                             MMC_RESET_TIMEOUT_MS);
  if (ret < 0)
    {
      _err("MMC: FIFO reset timeout\n");
    }

  return ret;
}

/****************************************************************************
 * Name: rk3506_mmc_ctrl_init
 *
 * Description:
 *   Controller initialisation, following HAL_MMC_Init()
 *   (hal_sdio.c:609-666) + u-boot dwmci_probe() FIFOTH programming.
 *
 ****************************************************************************/

static int rk3506_mmc_ctrl_init(void)
{
  int ret;

  ret = rk3506_mmc_reset();
  if (ret < 0)
    {
      return ret;
    }

  /* Fixed burst for the internal DMA master (DMA stays disabled; this is
   * what the HAL and Linux both leave set).
   */

  mmc_putreg(MMC_BMOD, mmc_getreg(MMC_BMOD) | BMOD_FB);

  /* No interrupts: polled driver.  Keep CTRL.INT_ENABLE set as the HAL
   * does, but mask everything.
   */

  mmc_putreg(MMC_RINTSTS, INT_ALL);
  mmc_putreg(MMC_INTMASK, 0);
  mmc_putreg(MMC_CTRL, mmc_getreg(MMC_CTRL) | CTRL_INT_ENABLE);

  /* Max timeout for all transfers (hal_sdio.c:642,663) */

  mmc_putreg(MMC_TMOUT, 0xffffffffu);

  /* FIFO thresholds for the RK3506 256-byte FIFO */

  mmc_putreg(MMC_FIFOTH, FIFOTH_VALUE);

  /* Power on, 1-bit until ACMD6 asks otherwise */

  mmc_putreg(MMC_PWREN, mmc_getreg(MMC_PWREN) | (1u << 0));
  mmc_putreg(MMC_CTYPE, CTYPE_WIDTH1);

  return OK;
}

/****************************************************************************
 * Name: rk3506_mmc_set_clock
 *
 * Description:
 *   Program the card clock.  Uses the exact CIU handshake of
 *   HAL_MMC_UpdateClockRegister() and Linux dw_mci_setup_bus():
 *     disable -> CLKSRC/CLKDIV -> CIU cmd -> enable -> CIU cmd
 *   with  card_clock = ciu / ((CLKDIV & 0xff) * 2).
 *
 ****************************************************************************/

static void rk3506_mmc_set_clock(uint32_t freq)
{
  uint32_t div;

  if (freq == 0)
    {
      /* Disable the card clock */

      mmc_putreg(MMC_CLKENA, 0);
      rk3506_mmc_send_ciu_cmd();
      return;
    }

  div = (CONFIG_RK3506_MMC_CIU_HZ + (2 * freq) - 1) / (2 * freq);
  if (div < 1)
    {
      div = 1;
    }

  if (div > 0xff)
    {
      div = 0xff;
    }

  /* Disable clock */

  mmc_putreg(MMC_CLKENA, 0);
  mmc_putreg(MMC_CLKSRC, 0);
  rk3506_mmc_send_ciu_cmd();

  /* Set clock to the desired speed */

  mmc_putreg(MMC_CLKDIV, div);
  rk3506_mmc_send_ciu_cmd();

  /* Enable clock (low power mode off: SD, not SDIO) */

  mmc_putreg(MMC_CLKENA, CLKENA_CCLK_ENABLE);
  rk3506_mmc_send_ciu_cmd();
}

/****************************************************************************
 * Name: rk3506_mmc_data
 *
 * Description:
 *   Perform the data phase of a command with polled FIFO (PIO) transfers.
 *
 *   Design note: IDMAC is deliberately not used.  PIO keeps the driver
 *   free of descriptor alignment and D-cache maintenance on Cortex-A7 and
 *   is the path the Rockchip HAL itself implements (HAL_MMC_ReadData /
 *   HAL_MMC_WriteData).  The controller back-pressures the card while the
 *   256-byte FIFO is full, so pumping from this (task) context is safe.
 *
 *   To switch to IDMAC later: build the bounce-aligned descriptors of
 *   HAL_MMC_InitDescriptors(), set BMOD.bit7 + CTRL.DMA_ENABLE, and finish
 *   on IDSTS rather than DTO.
 *
 ****************************************************************************/

static int rk3506_mmc_data(bool write)
{
  FAR struct rk3506_mmc_priv_s *priv = &g_mmc0;
  uint32_t rintsts;
  uint32_t words;
  uint32_t tail;
  size_t remaining;
  uint8_t lastword[4];
  clock_t start;
  clock_t ticks;

  remaining = priv->xfrlen;
  words     = remaining / 4;
  tail      = remaining & 3;

  start = clock_systime_ticks();
  ticks = MSEC2TICKS(MMC_DATA_TIMEOUT_MS);

  for (; ; )
    {
      uint32_t count;
      uint32_t n;

      rintsts = mmc_getreg(MMC_RINTSTS);

      if (rintsts & (INT_DATA_ERR | INT_DATA_TOUT))
        {
          _err("MMC: data error, RINTSTS=%08" PRIx32 "\n", rintsts);
          mmc_putreg(MMC_RINTSTS, INT_ALL);
          rk3506_mmc_reset_fifo();
          return (rintsts & INT_DATA_TOUT) ? -ETIMEDOUT : -EIO;
        }

      count = STATUS_FIFO_COUNT(mmc_getreg(MMC_STATUS));

      if (!write)
        {
          /* RX: drain everything the FIFO currently holds */

          n = count;
          if (n > words)
            {
              n = words;
            }

          while (n-- > 0)
            {
              uint32_t val = mmc_getreg(MMC_DATA);
              if (words > 0)
                {
                  memcpy(priv->rxbuffer, &val, 4);
                  priv->rxbuffer += 4;
                  words--;
                }
            }
        }
      else if (count < FIFO_DEPTH_WORDS)
        {
          /* TX: fill the free space */

          n = FIFO_DEPTH_WORDS - count;
          if (n > words)
            {
              n = words;
            }

          while (n-- > 0)
            {
              uint32_t val;
              memcpy(&val, priv->txbuffer, 4);
              priv->txbuffer += 4;
              words--;
              mmc_putreg(MMC_DATA, val);
            }
        }

      if (words == 0 && tail != 0)
        {
          /* Sub-word remainder: pad the last word (DW FIFO is 32-bit
           * only).  mmcsd only ever asks for 4-byte multiples, but the
           * SCR/CSD paths are cheap to keep correct.
           */

          if (write)
            {
              memset(lastword, 0, sizeof(lastword));
              memcpy(lastword, priv->txbuffer, tail);
              mmc_putreg(MMC_DATA,
                         ((uint32_t)lastword[0]) |
                         ((uint32_t)lastword[1] << 8) |
                         ((uint32_t)lastword[2] << 16) |
                         ((uint32_t)lastword[3] << 24));
            }
          else
            {
              uint32_t val = mmc_getreg(MMC_DATA);
              memcpy(lastword, &val, 4);
              memcpy(priv->rxbuffer, lastword, tail);
            }

          tail = 0;
        }

      /* Transfer over?  DTO is set when BYTCNT is exhausted; for a
       * multi-block transfer with CMD_SEND_STOP the auto CMD12 also
       * completes (ACD).
       */

      if (words == 0 && tail == 0 &&
          (rintsts & (INT_DTO | INT_ACD)) != 0)
        {
          mmc_putreg(MMC_RINTSTS, INT_ALL);

          /* For writes the card may still signal busy on DAT0; wait for
           * the data line to be released (HAL_MMC_WriteData tail loop).
           */

          if (write)
            {
              while ((mmc_getreg(MMC_STATUS) &
                      (STATUS_DATA_BUSY | STATUS_MC_BUSY)) != 0)
                {
                  if ((clock_systime_ticks() - start) >
                      (2 * ticks))
                    {
                      _err("MMC: data line stayed busy\n");
                      return -ETIMEDOUT;
                    }
                }
            }

          return OK;
        }

      if ((clock_systime_ticks() - start) > ticks)
        {
          _err("MMC: data transfer timeout, RINTSTS=%08" PRIx32 "\n",
               mmc_getreg(MMC_RINTSTS));
          mmc_putreg(MMC_RINTSTS, INT_ALL);
          rk3506_mmc_reset_fifo();
          return -ETIMEDOUT;
        }
    }
}

/****************************************************************************
 * Name: rk3506_mmc_sendcmd_sync
 *
 * Description:
 *   Issue one command (and, for data commands, run the whole data phase).
 *
 *   Order and flag construction follow u-boot dwmci_send_cmd()
 *   (dw_mmc.c:372-494):
 *     - wait STATUS.DATA_BUSY clear
 *     - RINTSTS = all (write-1-to-clear)
 *     - data: BLKSIZ, BYTCNT, FIFO reset
 *     - CMDARG, then CMD with START | USE_HOLD_REG | index | flags
 *     - poll RINTSTS for CDONE / RTO / RE
 *     - 136-bit response is reconstructed with the Linux dw_mmc shift
 *       (dw_mmc.c: RESPin are stored MSB-first with the start/transmission
 *       bits still in them).
 *
 ****************************************************************************/

static int rk3506_mmc_sendcmd_sync(uint32_t cmd, uint32_t arg)
{
  FAR struct rk3506_mmc_priv_s *priv = &g_mmc0;
  uint32_t index;
  uint32_t rspmode;
  uint32_t xfrmode;
  uint32_t flags;
  uint32_t rintsts;
  uint32_t nbytes;
  uint32_t blocksize;
  bool     data;
  bool     write;
  bool     multiblock;
  bool     stopcmd;
  int      ret;

  index  = cmd & MMCSD_CMDIDX_MASK;
  rspmode = cmd & MMCSD_RESPONSE_MASK;
  xfrmode = cmd & MMCSD_DATAXFR_MASK;

  data       = (xfrmode != 0);
  write      = (xfrmode & MMCSD_WRXFR) != 0;
  multiblock = (cmd & MMCSD_MULTIBLOCK) != 0;
  stopcmd    = (cmd & MMCSD_STOPXFR) != 0;

  /* A data command consumes the buffer recorded by recvsetup/sendsetup. */

  if (data && !priv->pendsetup)
    {
      _err("MMC: CMD%" PRIu32 " has data but no setup\n", index);
      return -EINVAL;
    }

  nbytes    = data ? priv->xfrlen : 0;
  blocksize = data ? CONFIG_RK3506_MMC_BLOCKSIZE : 0;

  if (data && nbytes < blocksize)
    {
      blocksize = nbytes;
    }

  /* A command may not be issued while the data FSM is still busy. */

  ret = rk3506_mmc_wait_bits(MMC_STATUS,
                             STATUS_DATA_BUSY | STATUS_MC_BUSY, 0,
                             MMC_CMD_TIMEOUT_MS);
  if (ret < 0)
    {
      _err("MMC: CMD%" PRIu32 " blocked by busy data FSM\n", index);
      return ret;
    }

  mmc_putreg(MMC_RINTSTS, INT_ALL);

  if (data)
    {
      mmc_putreg(MMC_BLKSIZ, blocksize);
      mmc_putreg(MMC_BYTCNT, nbytes);

      ret = rk3506_mmc_reset_fifo();
      if (ret < 0)
        {
          return ret;
        }
    }

  /* Command flags */

  flags = CMD_USE_HOLD | CMD_START | index;

  if (stopcmd)
    {
      /* CMD12: abort the running transfer */

      flags |= CMD_ABORT_STOP;
    }
  else
    {
      flags |= CMD_PRV_DAT_WAIT;
    }

  if (index == MMCSD_CMDIDX0)
    {
      flags |= CMD_SEND_INIT | CMD_ABORT_STOP;
    }

  if (rspmode != MMCSD_NO_RESPONSE)
    {
      flags |= CMD_RESP_EXP;

      if (rspmode == MMCSD_R2_RESPONSE)
        {
          flags |= CMD_RESP_LENGTH;
        }
      else
        {
          /* R1/R1b/R3/R4/R5/R6/R7 all carry a CRC with the R1/R6/R7
           * flavours; R3/R4 do not.  Only check CRC where the spec
           * provides one, matching u-boot's MMC_RSP_CRC mapping.
           */

          if (rspmode == MMCSD_R1_RESPONSE || rspmode == MMCSD_R1B_RESPONSE ||
              rspmode == MMCSD_R6_RESPONSE || rspmode == MMCSD_R7_RESPONSE)
            {
              flags |= CMD_CHECK_CRC;
            }
        }
    }

  if (data)
    {
      flags |= CMD_DATA_EXP;

      if (write)
        {
          flags |= CMD_RW;
        }

      /* Let the controller emit the stop command itself, otherwise the
       * card keeps streaming after BYTCNT is exhausted and overruns the
       * FIFO.
       */

      if (multiblock)
        {
          flags |= CMD_SEND_STOP;
        }
    }

  mmc_putreg(MMC_CMDARG, arg);
  mmc_putreg(MMC_CMD, flags);

  /* Wait for the command to complete */

  {
    clock_t start = clock_systime_ticks();
    clock_t ticks = MSEC2TICKS(MMC_CMD_TIMEOUT_MS);

    for (; ; )
      {
        rintsts = mmc_getreg(MMC_RINTSTS);

        if ((rintsts & INT_CDONE) != 0)
          {
            break;
          }

        if ((rintsts & (INT_RTO | INT_RE | INT_DCRC)) != 0)
          {
            break;
          }

        if ((clock_systime_ticks() - start) > ticks)
          {
            _err("MMC: CMD%" PRIu32 " timeout, RINTSTS=%08" PRIx32 "\n",
                 index, rintsts);
            mmc_putreg(MMC_RINTSTS, INT_ALL);
            return -ETIMEDOUT;
          }
      }
  }

  if ((rintsts & INT_RTO) != 0)
    {
      mmc_putreg(MMC_RINTSTS, INT_ALL);
      return -ETIMEDOUT;
    }

  if ((rintsts & (INT_RE | INT_DCRC)) != 0)
    {
      mmc_putreg(MMC_RINTSTS, INT_ALL);
      return -EIO;
    }

  /* Response */

  memset(priv->response, 0, sizeof(priv->response));

  if (rspmode == MMCSD_R2_RESPONSE)
    {
      uint32_t r0 = mmc_getreg(MMC_RESP0);
      uint32_t r1 = mmc_getreg(MMC_RESP1);
      uint32_t r2 = mmc_getreg(MMC_RESP2);
      uint32_t r3 = mmc_getreg(MMC_RESP3);

      priv->response[0] = (r3 << 8) | (r2 >> 24);
      priv->response[1] = (r2 << 8) | (r1 >> 24);
      priv->response[2] = (r1 << 8) | (r0 >> 24);
      priv->response[3] = (r0 << 8);
    }
  else if (rspmode != MMCSD_NO_RESPONSE)
    {
      priv->response[0] = mmc_getreg(MMC_RESP0);
    }

  if (!data)
    {
      mmc_putreg(MMC_RINTSTS, INT_ALL);
      priv->wkupevent = SDIOWAIT_CMDDONE |
                        (rspmode != MMCSD_NO_RESPONSE ?
                         SDIOWAIT_RESPONSEDONE : 0);
      return OK;
    }

  /* Data phase.  The pending setup is consumed either way. */

  priv->pendsetup = false;

  ret = rk3506_mmc_data(write);
  if (ret < 0)
    {
      priv->wkupevent = SDIOWAIT_ERROR |
                        (ret == -ETIMEDOUT ? SDIOWAIT_TIMEOUT : 0);
      return ret;
    }

  priv->wkupevent = SDIOWAIT_CMDDONE | SDIOWAIT_TRANSFERDONE |
                    (rspmode != MMCSD_NO_RESPONSE ?
                     SDIOWAIT_RESPONSEDONE : 0);

  return OK;
}

/****************************************************************************
 * Name: rk3506_mmc_present
 *
 * Description:
 *   Read the card-detect source configured by Kconfig.
 *
 ****************************************************************************/

static bool rk3506_mmc_present(void)
{
#ifdef CONFIG_RK3506_MMC_CD_GPIO
  bool level = false;

  if (rk3506_gpio_read(RK3506_GPIO_PIN(CONFIG_RK3506_MMC_CD_BANK,
                                       CONFIG_RK3506_MMC_CD_PIN),
                       &level) < 0)
    {
      return false;
    }

#ifdef CONFIG_RK3506_MMC_CD_ACTIVELOW
  return !level;
#else
  return level;
#endif
#else
  /* Controller-side CDETECT: bit 0 is active LOW */

  return (mmc_getreg(MMC_CDETECT) & CDETECT_CARD_PRESENT_N) == 0;
#endif
}

/****************************************************************************
 * sdio_dev_s callbacks
 ****************************************************************************/

static void rk3506_mmc_reset_cb(FAR struct sdio_dev_s *dev)
{
  mmc_putreg(MMC_PWREN, mmc_getreg(MMC_PWREN) & ~(1u << 0));
  rk3506_mmc_set_clock(0);
  rk3506_mmc_ctrl_init();

  g_mmc0.widebus   = false;
  g_mmc0.pendsetup = false;
  g_mmc0.wkupevent = 0;
}

static sdio_capset_t rk3506_mmc_capabilities(FAR struct sdio_dev_s *dev)
{
#if CONFIG_RK3506_MMC_BLOCKSIZE == 0
  return SDIO_CAPS_1BIT_ONLY;
#else
  return SDIO_CAPS_4BIT;
#endif
}

static sdio_statset_t rk3506_mmc_status(FAR struct sdio_dev_s *dev)
{
  sdio_statset_t status = 0;

  if (rk3506_mmc_present())
    {
      status |= SDIO_STATUS_PRESENT;
    }

  /* The board has no write-protect switch (Linux dts: disable-wp) */

  return status;
}

static void rk3506_mmc_widebus(FAR struct sdio_dev_s *dev, bool enable)
{
  g_mmc0.widebus = enable;
  mmc_putreg(MMC_CTYPE, enable ? CTYPE_WIDTH4 : CTYPE_WIDTH1);
}

static void rk3506_mmc_clock(FAR struct sdio_dev_s *dev,
                             enum sdio_clock_e rate)
{
  switch (rate)
    {
      case CLOCK_SDIO_DISABLED:
        rk3506_mmc_set_clock(0);
        break;

      case CLOCK_IDMODE:
        rk3506_mmc_set_clock(MMC_IDMODE_FREQ);
        break;

      default:
        rk3506_mmc_set_clock(CONFIG_RK3506_MMC_MAXFREQ);
        break;
    }
}

static int rk3506_mmc_attach(FAR struct sdio_dev_s *dev)
{
  /* No interrupt is used: commands and data complete synchronously inside
   * sendcmd() and are reported through the recorded event set.
   * drivers/mmcsd only requires a non-failing attach.
   */

  return OK;
}

static int rk3506_mmc_sendcmd(FAR struct sdio_dev_s *dev, uint32_t cmd,
                              uint32_t arg)
{
  return rk3506_mmc_sendcmd_sync(cmd, arg);
}

static int rk3506_mmc_recvsetup(FAR struct sdio_dev_s *dev,
                                FAR uint8_t *buffer, size_t nbytes)
{
  g_mmc0.rxbuffer  = buffer;
  g_mmc0.xfrlen    = nbytes;
  g_mmc0.pendwrite = false;
  g_mmc0.pendsetup = true;
  return nbytes;
}

static int rk3506_mmc_sendsetup(FAR struct sdio_dev_s *dev,
                                FAR const uint8_t *buffer, size_t nbytes)
{
  g_mmc0.txbuffer   = buffer;
  g_mmc0.xfrlen     = nbytes;
  g_mmc0.pendwrite  = true;
  g_mmc0.pendsetup  = true;
  return nbytes;
}

static int rk3506_mmc_cancel(FAR struct sdio_dev_s *dev)
{
  g_mmc0.pendsetup = false;
  g_mmc0.xfrlen    = 0;
  return OK;
}

static int rk3506_mmc_waitresponse(FAR struct sdio_dev_s *dev, uint32_t cmd)
{
  /* sendcmd() already waited for CDONE / the error bits. */

  if ((g_mmc0.wkupevent & (SDIOWAIT_ERROR | SDIOWAIT_TIMEOUT)) != 0)
    {
      return (g_mmc0.wkupevent & SDIOWAIT_TIMEOUT) ? -ETIMEDOUT : -EIO;
    }

  return OK;
}

static int rk3506_mmc_recv_rx(FAR struct sdio_dev_s *dev, uint32_t cmd,
                              FAR uint32_t *resp, int nwords)
{
  int i;

  for (i = 0; i < nwords; i++)
    {
      resp[i] = g_mmc0.response[i];
    }

  return OK;
}

static int rk3506_mmc_recv_r1(FAR struct sdio_dev_s *dev, uint32_t cmd,
                              FAR uint32_t *R1)
{
  return rk3506_mmc_recv_rx(dev, cmd, R1, 1);
}

static int rk3506_mmc_recv_r2(FAR struct sdio_dev_s *dev, uint32_t cmd,
                              FAR uint32_t R2[4])
{
  return rk3506_mmc_recv_rx(dev, cmd, R2, 4);
}

static int rk3506_mmc_recv_r3(FAR struct sdio_dev_s *dev, uint32_t cmd,
                              FAR uint32_t *R3)
{
  return rk3506_mmc_recv_rx(dev, cmd, R3, 1);
}

static int rk3506_mmc_recv_r4(FAR struct sdio_dev_s *dev, uint32_t cmd,
                              FAR uint32_t *R4)
{
  return rk3506_mmc_recv_rx(dev, cmd, R4, 1);
}

static int rk3506_mmc_recv_r5(FAR struct sdio_dev_s *dev, uint32_t cmd,
                              FAR uint32_t *R5)
{
  return rk3506_mmc_recv_rx(dev, cmd, R5, 1);
}

static int rk3506_mmc_recv_r6(FAR struct sdio_dev_s *dev, uint32_t cmd,
                              FAR uint32_t *R6)
{
  return rk3506_mmc_recv_rx(dev, cmd, R6, 1);
}

static int rk3506_mmc_recv_r7(FAR struct sdio_dev_s *dev, uint32_t cmd,
                              FAR uint32_t *R7)
{
  return rk3506_mmc_recv_rx(dev, cmd, R7, 1);
}

static void rk3506_mmc_waitenable(FAR struct sdio_dev_s *dev,
                                  sdio_eventset_t eventset, uint32_t timeout)
{
  /* Purely synchronous driver: remember what was asked for.  The result
   * was already latched by sendcmd().
   */

  g_mmc0.waitevents = eventset;
}

static sdio_eventset_t rk3506_mmc_eventwait(FAR struct sdio_dev_s *dev)
{
  sdio_eventset_t result = g_mmc0.wkupevent;

  g_mmc0.wkupevent  = 0;
  g_mmc0.waitevents = 0;

  /* SDIO_EVENTWAIT must always return a non-zero event set. */

  if (result == 0)
    {
      result = SDIOWAIT_TIMEOUT;
    }

  return result;
}

static void rk3506_mmc_callbackenable(FAR struct sdio_dev_s *dev,
                                      sdio_eventset_t eventset)
{
#ifdef CONFIG_SCHED_WORKQUEUE
  g_mmc0.cbevents = eventset;
#endif
}

#ifdef CONFIG_SCHED_WORKQUEUE
static int rk3506_mmc_registercallback(FAR struct sdio_dev_s *dev,
                                       worker_t callback, FAR void *arg)
{
  g_mmc0.callback = callback;
  g_mmc0.cbarg    = arg;
  g_mmc0.cbevents = 0;
  return OK;
}

static void rk3506_mmc_worker(FAR void *arg)
{
  FAR struct rk3506_mmc_priv_s *priv = arg;
  worker_t callback = priv->callback;
  FAR void *cbarg   = priv->cbarg;

  /* Events are automatically disabled once the callback runs. */

  priv->cbevents = 0;

  if (callback != NULL)
    {
      callback(cbarg);
    }
}
#endif

#ifdef CONFIG_SCHED_HPWORK
static void rk3506_mmc_notify(bool inserted)
{
  FAR struct rk3506_mmc_priv_s *priv = &g_mmc0;
  sdio_eventset_t wanted = inserted ? SDIOMEDIA_INSERTED :
                                      SDIOMEDIA_EJECTED;

  if (priv->callback == NULL || (priv->cbevents & wanted) == 0)
    {
      return;
    }

  work_queue(HPWORK, &priv->work, rk3506_mmc_worker, priv, 0);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_mmc_carddetect
 ****************************************************************************/

bool rk3506_mmc_carddetect(void)
{
  FAR struct rk3506_mmc_priv_s *priv = &g_mmc0;
  bool present;

  if (!g_mmc_registered)
    {
      return false;
    }

  present = rk3506_mmc_present();

#ifdef CONFIG_SCHED_HPWORK
  if (present != priv->present)
    {
      priv->present = present;
      rk3506_mmc_notify(present);
    }
#endif

  return present;
}

/****************************************************************************
 * Name: rk3506_mmc_sdio
 ****************************************************************************/

FAR struct sdio_dev_s *rk3506_mmc_sdio(void)
{
  return g_mmc_registered ? &g_mmc0.dev : NULL;
}

/****************************************************************************
 * Name: rk3506_mmc_initialize
 ****************************************************************************/

int rk3506_mmc_initialize(int slot)
{
  FAR struct rk3506_mmc_priv_s *priv = &g_mmc0;
  int ret;

  if (g_mmc_registered)
    {
      return OK;
    }

  memset(priv, 0, sizeof(*priv));

  /* The generic mutex in sdio_dev_s is used by drivers/mmcsd/sdio.c */

  nxmutex_init(&priv->dev.mutex);

  priv->dev.reset            = rk3506_mmc_reset_cb;
  priv->dev.capabilities     = rk3506_mmc_capabilities;
  priv->dev.status           = rk3506_mmc_status;
  priv->dev.widebus          = rk3506_mmc_widebus;
  priv->dev.clock            = rk3506_mmc_clock;
  priv->dev.attach           = rk3506_mmc_attach;
  priv->dev.sendcmd          = rk3506_mmc_sendcmd;
  priv->dev.recvsetup        = rk3506_mmc_recvsetup;
  priv->dev.sendsetup        = rk3506_mmc_sendsetup;
  priv->dev.cancel           = rk3506_mmc_cancel;
  priv->dev.waitresponse     = rk3506_mmc_waitresponse;
  priv->dev.recv_r1          = rk3506_mmc_recv_r1;
  priv->dev.recv_r2          = rk3506_mmc_recv_r2;
  priv->dev.recv_r3          = rk3506_mmc_recv_r3;
  priv->dev.recv_r4          = rk3506_mmc_recv_r4;
  priv->dev.recv_r5          = rk3506_mmc_recv_r5;
  priv->dev.recv_r6          = rk3506_mmc_recv_r6;
  priv->dev.recv_r7          = rk3506_mmc_recv_r7;
  priv->dev.waitenable       = rk3506_mmc_waitenable;
  priv->dev.eventwait        = rk3506_mmc_eventwait;
  priv->dev.callbackenable   = rk3506_mmc_callbackenable;
#ifdef CONFIG_SCHED_WORKQUEUE
  priv->dev.registercallback = rk3506_mmc_registercallback;
#endif

  priv->caps = rk3506_mmc_capabilities(&priv->dev);

  /* Clocks, then the controller */

  rk3506_mmc_clock_init();

  ret = rk3506_mmc_ctrl_init();
  if (ret < 0)
    {
      _err("MMC: controller init failed: %d\n", ret);
      return ret;
    }

  /* Identification clock before the MMCSD core starts talking to the card */

  rk3506_mmc_set_clock(MMC_IDMODE_FREQ);

  priv->present      = rk3506_mmc_present();
  g_mmc_registered   = true;

  ret = mmcsd_slotinitialize(slot, &priv->dev);

  /* -ENODEV here just means "no card in the slot right now"; the slot is
   * registered and mmcsd has armed SDIOMEDIA_INSERTED for hotplug.
   */

  if (ret < 0 && ret != -ENODEV)
    {
      _err("MMC: mmcsd_slotinitialize failed: %d\n", ret);
      g_mmc_registered = false;
      return ret;
    }

  _info("MMC: dw-mshc @%08" PRIx32 " registered as /dev/mmcsd%d, "
        "4-bit %" PRIu32 " Hz, card %s\n",
        RK3506_MMC_BASE, slot, CONFIG_RK3506_MMC_MAXFREQ,
        priv->present ? "present" : "absent");

  return OK;
}

#endif /* CONFIG_RK3506_MMC */
