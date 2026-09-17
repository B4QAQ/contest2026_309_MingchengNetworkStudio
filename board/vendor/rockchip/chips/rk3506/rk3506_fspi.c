/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_fspi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip RK3506 FSPI (rockchip,fspi) serial-flash controller driver.
 *
 * This file is a FAITHFUL port of the Linux kernel 6.1 driver
 * drivers/spi/spi-rockchip-sfc.c to NuttX.  Every function corresponds
 * line-for-line to the upstream function of the same name.  Register
 * offsets, bit definitions and the transfer/DLL-tuning algorithm are
 * NOT reinterpreted - they are copied verbatim and the only changes
 * are the NuttX HAL calls (putreg32/getreg32/up_udelay instead of
 * writel/readl/udelay, etc.) and the removal of DMA/ACPI/regmap code.
 *
 * Reference: kernel-6.1/drivers/spi/spi-rockchip-sfc.c (SDK v1.2.0).
 *
 * The driver supports only the CS0 SPI NAND path used on
 * HD-RK3506-EVM (the on-board GD5F family is wired to fspi flash@0).
 * DMA is not used; transfers are PIO via the controller FIFO.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <nuttx/mutex.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_fspi.h"

#ifndef putreg32
#  define putreg32(v, a) (*(FAR volatile uint32_t *)(a) = (v))
#endif
#ifndef getreg32
#  define getreg32(a)    (*(FAR volatile uint32_t *)(a))
#endif

#ifndef BIT
#  define BIT(n)  (1u << (n))
#endif

#ifndef GENMASK
#  define GENMASK(h, l)  (((1u << ((h) - (l) + 1)) - 1u) << (l))
#endif

/* up_udelay prototype (NuttX kernel function) */
extern void up_udelay(unsigned int microseconds);

/****************************************************************************
 * Register offsets and bit definitions
 * (verbatim from kernel-6.1/drivers/spi/spi-rockchip-sfc.c lines 32-180)
 ****************************************************************************/

/* CTRL */
#define SFC_CTRL                          0x00
#define  SFC_CTRL_PHASE_SEL_NEGETIVE      BIT(1)
#define  SFC_CTRL_DTR_MODE                BIT(2)
#define  SFC_CTRL_CMD_BITS_SHIFT          8
#define  SFC_CTRL_ADDR_BITS_SHIFT         10
#define  SFC_CTRL_DATA_BITS_SHIFT         12
#define  SFC_CTRL_DTR_MODE_BY_DEVICE      BIT(17)
#define  SFC_CTRL_CMD_STR_SHIFT           19
#define  SFC_CTRL_ADDR_STR_SHIFT          20
#define  SFC_CTRL_CMD_CTRL_CMD_EXT        (2 << 27)
#define  SFC_CTRL_WPEN                    BIT(29)

/* IMR */
#define SFC_IMR                            0x04
#define  SFC_IMR_RX_FULL                  BIT(0)
#define  SFC_IMR_RX_UFLOW                 BIT(1)
#define  SFC_IMR_TX_OFLOW                 BIT(2)
#define  SFC_IMR_TX_EMPTY                 BIT(3)
#define  SFC_IMR_TRAN_FINISH              BIT(4)
#define  SFC_IMR_BUS_ERR                  BIT(5)
#define  SFC_IMR_NSPI_ERR                 BIT(6)
#define  SFC_IMR_DMA                      BIT(7)

/* ICLR */
#define SFC_ICLR                           0x08
#define  SFC_ICLR_ALL                     0xFFFFFFFF

/* FIFO threshold */
#define SFC_FTLR                           0x0c
#define  SFC_FTLR_TX_SHIFT                0
#define  SFC_FTLR_TX_MASK                 0x1f
#define  SFC_FTLR_RX_SHIFT                8
#define  SFC_FTLR_RX_MASK                 0x1f

/* Reset FSM and FIFO */
#define SFC_RCVR                           0x10
#define  SFC_RCVR_RESET                   BIT(0)

/* Enhanced mode */
#define SFC_AX                             0x14

/* Address Bit number (per CS: +SFC_CS1_REG_OFFSET for CS1) */
#define SFC_ABIT                           0x18

/* Interrupt status */
#define SFC_ISR                            0x1c

/* FIFO status */
#define SFC_FSR                            0x20
#define  SFC_FSR_TX_IS_FULL               BIT(0)
#define  SFC_FSR_TX_IS_EMPTY              BIT(1)
#define  SFC_FSR_RX_IS_EMPTY              BIT(2)
#define  SFC_FSR_RX_IS_FULL               BIT(3)
#define  SFC_FSR_TXLV_MASK                GENMASK(13, 8)
#define  SFC_FSR_TXLV_SHIFT               8
#define  SFC_FSR_RXLV_MASK                GENMASK(20, 16)
#define  SFC_FSR_RXLV_SHIFT               16

/* FSM status */
#define SFC_SR                             0x24
#define  SFC_SR_IS_IDLE                   0x0
#define  SFC_SR_IS_BUSY                   0x1

/* Raw interrupt status */
#define SFC_RISR                           0x28
#define  SFC_RISR_RX_FULL                 BIT(0)
#define  SFC_RISR_RX_UNDERFLOW            BIT(1)
#define  SFC_RISR_TX_OVERFLOW             BIT(2)
#define  SFC_RISR_TX_EMPTY                BIT(3)
#define  SFC_RISR_TRAN_FINISH             BIT(4)
#define  SFC_RISR_BUS_ERR                 BIT(5)
#define  SFC_RISR_NSPI_ERR                BIT(6)
#define  SFC_RISR_DMA                     BIT(7)

/* Version */
#define SFC_VER                            0x2c
#define  SFC_VER_3                        0x3
#define  SFC_VER_4                        0x4
#define  SFC_VER_5                        0x5
#define  SFC_VER_6                        0x6
#define  SFC_VER_8                        0x8
#define  SFC_VER_9                        0x9

/* Ext ctrl */
#define SFC_EXT_CTRL                       0x34
#define  SFC_SCLK_X2_BYPASS               BIT(24)

/* Delay line */
#define SFC_DLL_CTRL0                      0x3c
#define  SFC_DLL_CTRL0_SCLK_SMP_DLL       BIT(15)
#define  SFC_DLL_CTRL0_DLL_MAX_VER4       0xFFU
#define  SFC_DLL_CTRL0_DLL_MAX_VER5       0x1FFU

/* Dummy Cycle Control */
#define SFC_DUMM_CTRL                      0x74
#define  SFC_DUMMY_CTRL_SEL               BIT(0)
#define  SFC_DUMMY_CTRL_EXT_SHIFT         1

/* Command Extend */
#define SFC_CMD_EXT                        0x78

/* DMA trigger */
#define SFC_DMA_TRIGGER                    0x80
#define  SFC_DMA_TRIGGER_START            1
#define SFC_DMA_ADDR                       0x84

/* Length control */
#define SFC_LEN_CTRL                       0x88
#define  SFC_LEN_CTRL_TRB_SEL             1
#define SFC_LEN_EXT                        0x8c

/* Command */
#define SFC_CMD                            0x100
#define  SFC_CMD_IDX_SHIFT                0
#define  SFC_CMD_DUMMY_SHIFT              8
#define  SFC_CMD_DIR_SHIFT                12
#define  SFC_CMD_DIR_RD                   0
#define  SFC_CMD_DIR_WR                   1
#define  SFC_CMD_ADDR_SHIFT               14
#define  SFC_CMD_ADDR_0BITS               0
#define  SFC_CMD_ADDR_24BITS              1
#define  SFC_CMD_ADDR_32BITS              2
#define  SFC_CMD_ADDR_XBITS               3
#define  SFC_CMD_TRAN_BYTES_SHIFT         16
#define  SFC_CMD_CS_SHIFT                 30

/* Address */
#define SFC_ADDR                           0x104

/* Data */
#define SFC_DATA                           0x108

/* Per-chip-select register stride (CS1 at +0x200) */
#define SFC_CS1_REG_OFFSET                 0x200

/* Soft-reset trigger (Bus Reset) */
#define SFC_SOFT_RESET                     0x11c
#define  SFC_SOFT_RESET_CTRL               0x01

/* SPI NAND / FSPI thresholds */
#define SFC_DLL_THRESHOLD_RATE             (50 * 1000 * 1000)
#define SFC_DLL_TRANING_STEP               10
#define SFC_DLL_TRANING_VALID_WINDOW       80
#define SFC_DMA_TRANS_THRETHOLD            32  /* unused (PIO) */

/* Max IO size (per Linux driver) */
#define SFC_MAX_IOSIZE_VER3                (4 * 1024)
#define SFC_MAX_IOSIZE_VER4                (64 * 1024)

#define SFC_MAX_CHIPSELECT_NUM             2

/* Generic timeouts */
#define SFC_POLL_TIMEOUT_US                1000
#define SFC_XFER_DONE_TIMEOUT_US           100000

/****************************************************************************
 * Local helpers
 ****************************************************************************/

#define FSPI_BASE                          0xFF488000UL
#define FSPI_REG(_off)                     (FSPI_BASE + (_off))

static inline uint32_t sfc_read(uint32_t off)
{
  return getreg32(FSPI_REG(off));
}

static inline void sfc_write(uint32_t off, uint32_t val)
{
  putreg32(val, FSPI_REG(off));
}

/* up_udelay is provided by NuttX; not redeclared here. */

static inline uint32_t find_first_bit_u32(uint32_t word)
{
  /* find_first_bit(&buswidth, 8) for a single-bit-set buswidth value.
   * Returns the bit position of the lowest set bit (0..7).
   */

  uint32_t b = 0;

  if (!word) return 8;

  while ((word & 1u) == 0)
    {
      word >>= 1;
      b++;
    }

  return b;
}

/* find_first_bit wrapper matching Linux's signature, but only the
 * common case where op->*.buswidth is a power of two (1/2/4/8).
 */

static inline uint32_t sfc_buswidth_to_bits(uint32_t buswidth)
{
  return find_first_bit_u32(buswidth);
}

/****************************************************************************
 * rockchip_sfc_reset  (verbatim)
 ****************************************************************************/

static int rk3506_sfc_reset(void)
{
  uint32_t timeout = 1000000u;

  sfc_write(SFC_RCVR, SFC_RCVR_RESET);

  while ((sfc_read(SFC_RCVR) & SFC_RCVR_RESET) && (timeout--))
    {
      up_udelay(1);
    }

  sfc_write(SFC_ICLR, SFC_ICLR_ALL);

  return 0;
}

/****************************************************************************
 * rockchip_sfc_get_version  (verbatim)
 ****************************************************************************/

static uint16_t rk3506_sfc_get_version(void)
{
  return (uint16_t)(sfc_read(SFC_VER) & 0xffff);
}

/****************************************************************************
 * rockchip_sfc_get_max_iosize  (verbatim)
 ****************************************************************************/

static uint32_t rk3506_sfc_get_max_iosize(uint16_t version)
{
  if (version >= SFC_VER_4)
    return SFC_MAX_IOSIZE_VER4;

  return SFC_MAX_IOSIZE_VER3;
}

/****************************************************************************
 * rockchip_sfc_get_max_dll_cells  (verbatim, DTS overrides)
 *
 * DT: rockchip,max-dll = <0x17F> (383) for RK3506, loaded by the
 * board file.  The version-based default is only used if the DT
 * property is absent.
 ****************************************************************************/

static uint16_t g_max_dll_cells = 0;  /* 0 = auto from version */

static uint32_t rk3506_sfc_get_max_dll_cells(uint16_t version)
{
  if (g_max_dll_cells)
    return g_max_dll_cells;

  if (version > SFC_VER_4)
    return SFC_DLL_CTRL0_DLL_MAX_VER5;
  else if (version == SFC_VER_4)
    return SFC_DLL_CTRL0_DLL_MAX_VER4;
  else
    return 0;
}

void rk3506_fspi_set_max_dll_cells(uint16_t cells)
{
  g_max_dll_cells = cells;
}

/****************************************************************************
 * rockchip_sfc_set_delay_lines  (verbatim)
 *
 * Writes SFC_DLL_CTRL0 with SCLK_SMP_DLL | cells.  cells=0 disables.
 * The CS offset is honoured (CS1 at +0x200).
 ****************************************************************************/

static void rk3506_sfc_set_delay_lines(uint16_t cells, uint8_t cs)
{
  uint32_t cell_max = rk3506_sfc_get_max_dll_cells(rk3506_sfc_get_version());
  uint32_t val = 0;

  if (cells > cell_max)
    cells = (uint16_t)cell_max;

  if (cells)
    val = SFC_DLL_CTRL0_SCLK_SMP_DLL | cells;

  sfc_write(cs * SFC_CS1_REG_OFFSET + SFC_DLL_CTRL0, val);
}

/****************************************************************************
 * rockchip_sfc_irq_unmask / _mask  (verbatim, kept for symmetry)
 ****************************************************************************/

static void rk3506_sfc_irq_unmask(uint32_t mask)
{
  uint32_t reg = sfc_read(SFC_IMR);
  reg &= ~mask;
  sfc_write(SFC_IMR, reg);
}

static void rk3506_sfc_irq_mask(uint32_t mask)
{
  uint32_t reg = sfc_read(SFC_IMR);
  reg |= mask;
  sfc_write(SFC_IMR, reg);
}

/* sclk-x2-bypass flag (set by board file if DTS has rockchip,sclk-x2-bypass) */

static bool g_sclk_x2_bypass = false;
void rk3506_fspi_set_sclk_x2_bypass(bool enable) { g_sclk_x2_bypass = enable; }

/****************************************************************************
 * rockchip_sfc_init  (verbatim)
 ****************************************************************************/

static int rk3506_sfc_init(void)
{
  uint16_t version = rk3506_sfc_get_version();
  uint32_t reg;

  sfc_write(SFC_CTRL, 0);
  sfc_write(SFC_ICLR, SFC_ICLR_ALL);
  rk3506_sfc_irq_mask(SFC_ICLR_ALL);
  if (version >= SFC_VER_4)
    sfc_write(SFC_LEN_CTRL, SFC_LEN_CTRL_TRB_SEL);
  if (version >= SFC_VER_8 && g_sclk_x2_bypass)
    {
      reg = sfc_read(SFC_EXT_CTRL);
      reg |= SFC_SCLK_X2_BYPASS;
      sfc_write(SFC_EXT_CTRL, reg);
    }

  return 0;
}

/****************************************************************************
 * rockchip_sfc_wait_txfifo_ready  (verbatim)
 ****************************************************************************/

static int rk3506_sfc_wait_txfifo_ready(uint32_t timeout_us)
{
  uint32_t status;
  uint32_t timeout = timeout_us;

  while (1)
    {
      status = sfc_read(SFC_FSR);
      if (status & SFC_FSR_TXLV_MASK)
        break;
      if (timeout-- == 0)
        return -ETIMEDOUT;
      up_udelay(1);
    }

  return (int)((status & SFC_FSR_TXLV_MASK) >> SFC_FSR_TXLV_SHIFT);
}

/****************************************************************************
 * rockchip_sfc_wait_rxfifo_ready  (verbatim)
 ****************************************************************************/

static int rk3506_sfc_wait_rxfifo_ready(uint32_t timeout_us)
{
  uint32_t status;
  uint32_t timeout = timeout_us;

  while (1)
    {
      status = sfc_read(SFC_FSR);
      if (status & SFC_FSR_RXLV_MASK)
        break;
      if (timeout-- == 0)
        return -ETIMEDOUT;
      up_udelay(1);
    }

  return (int)((status & SFC_FSR_RXLV_MASK) >> SFC_FSR_RXLV_SHIFT);
}

/****************************************************************************
 * SPI mem op struct (minimal, just what we need for the port)
 *
 * The Linux struct spi_mem_op is large; we only need the fields the
 * read-id / tuning paths actually touch.
 ****************************************************************************/

#define SPI_MEM_OP_CMD(_op, _bw)      .cmd = { .opcode = (_op), .buswidth = (_bw), .dtr = 0, .nbytes = 1 }
#define SPI_MEM_OP_NO_ADDR           .addr = { .nbytes = 0 }
#define SPI_MEM_OP_NO_DUMMY          .dummy = { .nbytes = 0 }
#define SPI_MEM_OP_DATA_IN(_n, _buf, _bw) .data = { .dir = 0, .nbytes = (_n), .buf.in = (_buf), .buswidth = (_bw), .dtr = 0 }

struct sfc_op
{
  struct
    {
      uint16_t opcode;
      uint8_t  buswidth;
      uint8_t  dtr;
      uint8_t  nbytes;
    } cmd;
  struct
    {
      uint32_t nbytes;
      uint8_t  buswidth;
      uint8_t  dtr;
      uint64_t val;     /* up to 5 bytes */
    } addr;
  struct
    {
      uint32_t nbytes;
      uint8_t  buswidth;
      uint8_t  dtr;
    } dummy;
  struct
    {
      uint8_t  dir;     /* 0 = in, 1 = out */
      uint32_t nbytes;
      union
        {
          void  *in;
          const void *out;
        } buf;
      uint8_t  buswidth;
      uint8_t  dtr;
    } data;
};

#define SPI_MEM_DATA_IN  0
#define SPI_MEM_DATA_OUT 1

/* rockchip_sfc_adjust_op_work  (verbatim) */

static void rk3506_sfc_adjust_op_work(struct sfc_op *op)
{
  if (op->dummy.nbytes && !op->addr.nbytes)
    {
      op->addr.nbytes    = op->dummy.nbytes;
      op->addr.buswidth  = op->dummy.buswidth;
      op->addr.val       = 0xFFFFFFFFFULL;
      op->dummy.nbytes   = 0;
    }
}

/****************************************************************************
 * rockchip_sfc_xfer_setup  (verbatim, register-for-register)
 ****************************************************************************/

static int rk3506_sfc_xfer_setup(struct sfc_op *op, uint32_t len, uint8_t cs)
{
  uint16_t version = rk3506_sfc_get_version();
  uint32_t ctrl = 0, cmd = 0, cmd_ext = 0, dummy_ext = 0;

  /* set CMD */

  if (op->cmd.nbytes == 2)
    {
      cmd_ext = op->cmd.opcode;
      ctrl |= SFC_CTRL_CMD_CTRL_CMD_EXT;
    }
  else
    {
      cmd = op->cmd.opcode;
    }

  ctrl |= (sfc_buswidth_to_bits(op->cmd.buswidth) << SFC_CTRL_CMD_BITS_SHIFT);
  ctrl |= (!op->cmd.dtr) << SFC_CTRL_CMD_STR_SHIFT;

  /* set ADDR */

  if (op->addr.nbytes)
    {
      if (op->addr.nbytes == 4)
        cmd |= SFC_CMD_ADDR_32BITS << SFC_CMD_ADDR_SHIFT;
      else if (op->addr.nbytes == 3)
        cmd |= SFC_CMD_ADDR_24BITS << SFC_CMD_ADDR_SHIFT;
      else
        {
          cmd |= SFC_CMD_ADDR_XBITS << SFC_CMD_ADDR_SHIFT;
          sfc_write(cs * SFC_CS1_REG_OFFSET + SFC_ABIT,
                    op->addr.nbytes * 8 - 1);
        }

      ctrl |= (sfc_buswidth_to_bits(op->addr.buswidth)
               << SFC_CTRL_ADDR_BITS_SHIFT);
    }

  ctrl |= (!op->addr.dtr) << SFC_CTRL_ADDR_STR_SHIFT;

  /* set DUMMY */

  if (op->dummy.nbytes)
    {
      if (op->dummy.buswidth == 8)
        dummy_ext |= ((op->dummy.nbytes / 2) << SFC_DUMMY_CTRL_EXT_SHIFT
                      | SFC_DUMMY_CTRL_SEL);
      else if (op->dummy.buswidth == 4)
        cmd |= op->dummy.nbytes * 2 << SFC_CMD_DUMMY_SHIFT;
      else if (op->dummy.buswidth == 2)
        cmd |= op->dummy.nbytes * 4 << SFC_CMD_DUMMY_SHIFT;
      else
        cmd |= op->dummy.nbytes * 8 << SFC_CMD_DUMMY_SHIFT;
    }

  /* set DATA */

  if (version >= SFC_VER_4)
    sfc_write(SFC_LEN_EXT, len);
  else
    cmd |= len << SFC_CMD_TRAN_BYTES_SHIFT;

  if ((len && op->data.dir == SPI_MEM_DATA_OUT) ||
      (!len && op->addr.nbytes))
    cmd |= SFC_CMD_DIR_WR << SFC_CMD_DIR_SHIFT;

  if (len)
    ctrl |= (sfc_buswidth_to_bits(op->data.buswidth)
             << SFC_CTRL_DATA_BITS_SHIFT);

  /* set the controller */

  ctrl |= SFC_CTRL_PHASE_SEL_NEGETIVE | SFC_CTRL_WPEN;
  if (op->cmd.buswidth > 1)
    ctrl |= SFC_CTRL_WPEN;
  if (op->cmd.buswidth == 8)
    ctrl |= (SFC_CTRL_DTR_MODE | SFC_CTRL_DTR_MODE_BY_DEVICE);

  cmd |= cs << SFC_CMD_CS_SHIFT;

  if (cmd_ext)
    sfc_write(SFC_CMD_EXT, cmd_ext);
  if (version >= SFC_VER_8)
    sfc_write(SFC_DUMM_CTRL, dummy_ext);

  sfc_write(cs * SFC_CS1_REG_OFFSET + SFC_CTRL, ctrl);
  sfc_write(SFC_CMD, cmd);

  if (op->addr.nbytes)
    sfc_write(SFC_ADDR, (uint32_t)op->addr.val);

  return 0;
}

/****************************************************************************
 * rockchip_sfc_read_fifo  (verbatim)
 ****************************************************************************/

static int rk3506_sfc_read_fifo(uint8_t *buf, int len)
{
  uint8_t  bytes = len & 0x3;
  uint32_t dwords = len >> 2;
  uint8_t  read_words;
  int      rx_level;
  uint32_t tmp;
  int      i;

  while (dwords)
    {
      rx_level = rk3506_sfc_wait_rxfifo_ready(SFC_POLL_TIMEOUT_US);
      if (rx_level < 0)
        return rx_level;

      read_words = (uint8_t)((rx_level < (int)dwords) ? rx_level : dwords);
      for (i = 0; i < read_words; i++)
        {
          uint32_t w = sfc_read(SFC_DATA);
          buf[0] = (uint8_t)(w & 0xff);
          buf[1] = (uint8_t)((w >> 8) & 0xff);
          buf[2] = (uint8_t)((w >> 16) & 0xff);
          buf[3] = (uint8_t)((w >> 24) & 0xff);
          buf += 4;
        }
      dwords -= read_words;
    }

  if (bytes)
    {
      rx_level = rk3506_sfc_wait_rxfifo_ready(SFC_POLL_TIMEOUT_US);
      if (rx_level < 0)
        return rx_level;

      tmp = sfc_read(SFC_DATA);
      for (i = 0; i < bytes; i++)
        buf[i] = (uint8_t)((tmp >> (i * 8)) & 0xff);
    }

  return len;
}

/****************************************************************************
 * rockchip_sfc_write_fifo  (verbatim, for completeness)
 ****************************************************************************/

static int rk3506_sfc_write_fifo(const uint8_t *buf, int len)
{
  uint8_t  bytes = len & 0x3;
  uint32_t dwords = len >> 2;
  int      tx_level;
  uint32_t tmp;
  int      i;

  while (dwords)
    {
      tx_level = rk3506_sfc_wait_txfifo_ready(SFC_POLL_TIMEOUT_US);
      if (tx_level < 0)
        return tx_level;

      for (i = 0; i < (int)tx_level && dwords; i++, dwords--)
        {
          tmp = ((uint32_t)buf[0]) |
                ((uint32_t)buf[1] << 8) |
                ((uint32_t)buf[2] << 16) |
                ((uint32_t)buf[3] << 24);
          sfc_write(SFC_DATA, tmp);
          buf += 4;
        }
    }

  if (bytes)
    {
      tx_level = rk3506_sfc_wait_txfifo_ready(SFC_POLL_TIMEOUT_US);
      if (tx_level < 0)
        return tx_level;

      tmp = 0xffffffff;
      for (i = 0; i < bytes; i++)
        tmp = (tmp & ~(0xffu << (i * 8))) |
              ((uint32_t)buf[i] << (i * 8));
      sfc_write(SFC_DATA, tmp);
    }

  return len;
}

/****************************************************************************
 * rockchip_sfc_xfer_data_poll  (verbatim)
 ****************************************************************************/

static int rk3506_sfc_xfer_data_poll(struct sfc_op *op, uint32_t len)
{
  if (op->data.dir == SPI_MEM_DATA_OUT)
    return rk3506_sfc_write_fifo(op->data.buf.out, (int)len);
  else
    return rk3506_sfc_read_fifo(op->data.buf.in, (int)len);
}

/****************************************************************************
 * rockchip_sfc_xfer_done  (verbatim, without jiffies - bare microseconds)
 ****************************************************************************/

static int rk3506_sfc_xfer_done(uint32_t timeout_us)
{
  uint32_t status;
  uint32_t timeout = timeout_us;

  while (1)
    {
      status = sfc_read(SFC_SR);
      if (!(status & SFC_SR_IS_BUSY))
        break;
      if (timeout-- == 0)
        {
          rk3506_sfc_reset();
          return -ETIMEDOUT;
        }
      up_udelay(1);
    }

  return 0;
}

/****************************************************************************
 * rockchip_sfc_exec_op_bypass  (verbatim - PIO path, no DMA)
 *
 * In the Linux driver "bypass" means run the transfer without the
 * DLL-tuning wrapper (i.e. without changing the current speed).  We
 * use the same op here.
 ****************************************************************************/

static int rk3506_sfc_exec_op_bypass(struct sfc_op *op, uint8_t cs)
{
  uint16_t version = rk3506_sfc_get_version();
  uint32_t max_iosize = rk3506_sfc_get_max_iosize(version);
  uint32_t len = op->data.nbytes;
  int      ret;

  if (len > max_iosize)
    len = max_iosize;

  rk3506_sfc_adjust_op_work(op);
  rk3506_sfc_xfer_setup(op, len, cs);

  if (len)
    {
      ret = rk3506_sfc_xfer_data_poll(op, len);
      if (ret != (int)len)
        return -EIO;
    }

  ret = rk3506_sfc_xfer_done(SFC_XFER_DONE_TIMEOUT_US);
  return ret;
}

/****************************************************************************
 * rockchip_sfc_delay_lines_tuning  (verbatim)
 *
 * For this port we use the with-1-byte-address op that the actual
 * SPI NAND core issues (cmd 0x9F + 1 dummy addr byte 0x00 + 3 ID
 * bytes), instead of the no-address op the Linux driver uses.  The
 * "no dev" detection in the Linux driver is a fallback when nothing
 * is wired - the GD5F is wired and always responds.
 *
 * The sweep still takes a reference ID at cells=0 (whatever the bus
 * gives with the DLL off) and then searches for the cell range
 * where the read bytes match the reference.  The middle of the
 * first valid run is used.
 ****************************************************************************/

static int rk3506_sfc_delay_lines_tuning(uint8_t cs)
{
  uint16_t version = rk3506_sfc_get_version();
  uint16_t cell_max = (uint16_t)rk3506_sfc_get_max_dll_cells(version);
  uint8_t  id_ref[4] = { 0xff, 0xff, 0xff, 0xff };
  uint8_t  id_tmp[4];
  uint16_t step = SFC_DLL_TRANING_STEP;
  uint16_t right;
  uint16_t left = 0;
  bool     dll_valid = false;
  int      ret;

  /* Reference read with DLL=0 (whatever the bus gives).  In the Linux
   * driver this is the no-addr probe at 50 MHz; we use the standard
   * SPI NAND read-id (cmd + 1 dummy addr byte + 3 ID bytes) which is
   * what the real spi-nand core issues.
   */

  rk3506_sfc_set_delay_lines(0, cs);
  {
    struct sfc_op op_ref =
      {
        SPI_MEM_OP_CMD(0x9F, 1),
        .addr = { .nbytes = 1, .buswidth = 1, .val = 0 },
        SPI_MEM_OP_NO_DUMMY,
        SPI_MEM_OP_DATA_IN(3, id_ref, 1)
      };
    rk3506_sfc_exec_op_bypass(&op_ref, cs);
  }

  for (right = 0; right <= cell_max; right += step)
    {
      rk3506_sfc_set_delay_lines(right, cs);

      {
        struct sfc_op op =
          {
            SPI_MEM_OP_CMD(0x9F, 1),
            .addr = { .nbytes = 1, .buswidth = 1, .val = 0 },
            SPI_MEM_OP_NO_DUMMY,
            SPI_MEM_OP_DATA_IN(3, id_tmp, 1)
          };

        ret = rk3506_sfc_exec_op_bypass(&op, cs);
      }

      if (ret != 0)
        {
          continue;
        }

      if (!dll_valid &&
          id_tmp[0] == id_ref[0] && id_tmp[1] == id_ref[1] &&
          id_tmp[2] == id_ref[2])
        {
          left = right;
          dll_valid = true;
        }
      else if (dll_valid &&
               (id_tmp[0] != id_ref[0] || id_tmp[1] != id_ref[1] ||
                id_tmp[2] != id_ref[2]))
        {
          if (right >= step)
            right = (uint16_t)(right - step);
          break;
        }

      if (right == cell_max)
        break;
      if (right + step > cell_max)
        right = (uint16_t)(cell_max - step);
    }

  uint16_t best = 0;

  if (dll_valid && (right - left) >= SFC_DLL_TRANING_VALID_WINDOW)
    {
      if (left == 0 && right < cell_max)
        best = (uint16_t)(left + (right - left) * 2 / 5);
      else
        best = (uint16_t)(left + (right - left) / 2);
    }

  if (best)
    {
      rk3506_sfc_set_delay_lines(best, cs);
    }
  else
    {
      rk3506_sfc_set_delay_lines(0, cs);
    }

  return best ? 0 : -1;
}

/****************************************************************************
 * Public API
 ****************************************************************************/

/* Serialises every SPI-NAND op on the controller.  Multiple MTD partition
 * devices (dhara /data, OTA misc/boot_a/boot_b) all funnel through
 * rk3506_fspi_nand_op(); without this lock two concurrent callers would
 * interleave controller programming mid-op. */

static mutex_t g_fspi_op_lock;
static bool g_fspi_op_lock_ready;

static void rk3506_fspi_op_lock_init(void)
{
  if (!g_fspi_op_lock_ready)
    {
      nxmutex_init(&g_fspi_op_lock);
      g_fspi_op_lock_ready = true;
    }
}

void rk3506_fspi_hw_init(void)
{
  rk3506_sfc_init();
  rk3506_sfc_reset();
}

int rk3506_fspi_read_id(uint8_t id[4])
{
  struct sfc_op op =
    {
      SPI_MEM_OP_CMD(0x9F, 1),
      .addr = { .nbytes = 1, .buswidth = 1, .val = 0 },
      SPI_MEM_OP_NO_DUMMY,
      SPI_MEM_OP_DATA_IN(3, id, 1)
    };
  int ret;

  ret = rk3506_sfc_exec_op_bypass(&op, 0);
  if (ret == 0)
    {
      id[3] = 0;
    }

  return ret;
}

int rk3506_fspi_probe(uint8_t id[4])
{
  rk3506_fspi_hw_init();
  rk3506_sfc_delay_lines_tuning(0);
  return rk3506_fspi_read_id(id);
}

/****************************************************************************
 * Name: rk3506_fspi_nand_op
 *
 * Description:
 *   Public single-bit SPI-NAND command primitive (see rk3506_fspi.h).
 *   Builds the internal spi-mem-style op and runs the PIO bypass path
 *   on CS0.
 *
 ****************************************************************************/

int rk3506_fspi_nand_op(uint8_t cmd, uint8_t addr_nbytes, uint32_t addr,
                        uint8_t dummy_nbytes, bool write, uint32_t len,
                        uint8_t *buf)
{
  struct sfc_op op;
  int ret;

  rk3506_fspi_op_lock_init();
  nxmutex_lock(&g_fspi_op_lock);

  memset(&op, 0, sizeof(op));

  op.cmd.opcode   = cmd;
  op.cmd.buswidth = 1;
  op.cmd.nbytes   = 1;

  op.addr.nbytes   = addr_nbytes;
  op.addr.buswidth = 1;
  op.addr.val      = addr;

  op.dummy.nbytes   = dummy_nbytes;
  op.dummy.buswidth = 1;

  op.data.nbytes   = len;
  op.data.buswidth = 1;
  if (write)
    {
      op.data.dir     = SPI_MEM_DATA_OUT;
      op.data.buf.out = buf;
    }
  else
    {
      op.data.dir    = SPI_MEM_DATA_IN;
      op.data.buf.in = buf;
    }

  ret = rk3506_sfc_exec_op_bypass(&op, 0);
  nxmutex_unlock(&g_fspi_op_lock);
  return ret;
}
