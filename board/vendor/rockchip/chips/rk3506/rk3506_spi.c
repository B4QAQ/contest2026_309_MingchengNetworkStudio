/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_spi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip RK3506 SPI controller driver (rk_spi / "rockchip,rk3066-spi").
 *
 * This is a polling-only driver for the RK3506 SPI0 / SPI1 controllers
 * (register base 0xFF120000 / 0xFF130000, IRQ 75 / 76).  It exposes them
 * to NuttX through the standard spi_ops_s interface so upper-half drivers
 * (gd5f SPI NAND, ...) can attach.
 *
 * IMPORTANT: register layout and bit semantics are those of the Rockchip
 * rk_spi IP (struct SPI_REG in the SDK rk3506.h, hal/lib/hal/src/hal_spi.c),
 * NOT a generic DesignWare SSI.  The two differ in ways that previously
 * caused a hard hang:
 *
 *   - TX data register is at offset 0x400, RX at 0x800 (not 0x60/0x64).
 *   - SR bit 1 is TFF (TX FIFO FULL) and bit 3 is RFE (RX FIFO EMPTY),
 *     i.e. the opposite polarity of the DesignWare TFNF/RFNE flags.
 *   - CTRLR0 DFS is a 2-bit field [1:0] (0=4b,1=8b,2=16b) and the master
 *     framing uses EM/BHT/SSD/XFM/OPM fields, not FRF/TMOD.
 *   - BAUDR divides the source clock directly (sclk = spiclk / BAUDR,
 *     BAUDR rounded up to an even number), not spiclk/(BAUDR+1).
 *
 * FIFO room/level is taken from the TXFLR/RXFLR counters as the SDK HAL
 * does.  Every poll has a bounded timeout: if the controller never moves
 * (wrong pinmux/clock, no device), the transfer returns with 0xFF fill
 * instead of hanging the boot.
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
#include <nuttx/spinlock.h>
#include <nuttx/spi/spi.h>
#include <nuttx/kmalloc.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_spi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef putreg32
#  define putreg32(v, x) (*(FAR volatile uint32_t *)(x) = (v))
#endif

#ifndef getreg32
#  define getreg32(x) (*(FAR volatile uint32_t *)(x))
#endif

#define RK3506_SPI_NUM_BUS        2

/* Default SPI clock rate: 1 MHz, well within the GD5F1GQ4's cap */

#define SPI_FREQ_DEFAULT          1000000
#define SPI_FREQ_MAX              50000000

/* FIFO depth (rk_spi on RK3506 is 64 words; HAL_SPI_FIFO_LENGTH) */

#define SPI_FIFO_DEPTH            64

/* Register offsets (struct SPI_REG, SDK rk3506.h) */

#define SPI_REG_CTRLR0            0x00
#define SPI_REG_CTRLR1            0x04
#define SPI_REG_ENR               0x08
#define SPI_REG_SER               0x0c
#define SPI_REG_BAUDR             0x10
#define SPI_REG_TXFTLR            0x14
#define SPI_REG_RXFTLR            0x18
#define SPI_REG_TXFLR             0x1c
#define SPI_REG_RXFLR             0x20
#define SPI_REG_SR                0x24
#define SPI_REG_IMR               0x2c
#define SPI_REG_ICR               0x38
#define SPI_REG_DMACR             0x3c
#define SPI_REG_TXDR              0x400   /* TX FIFO data (write) */
#define SPI_REG_RXDR              0x800   /* RX FIFO data (read)  */

/* CTRLR0 fields (SDK hal_spi.h CR0_* values) */

#define SPI_CR0_DFS_4BIT          (0x00u << 0)
#define SPI_CR0_DFS_8BIT          (0x01u << 0)
#define SPI_CR0_DFS_16BIT         (0x02u << 0)
#define SPI_CR0_SCPH              (1u << 6)   /* clock phase   */
#define SPI_CR0_SCPOL             (1u << 7)   /* clock polarity */
#define SPI_CR0_CSM_0             (0x00u << 8)
#define SPI_CR0_SSD_ONE           (1u << 10)  /* 1 sclk SS setup */
#define SPI_CR0_EM_BIG            (1u << 11)  /* big endian     */
#define SPI_CR0_FBM_MSB           (0x00u << 12)
#define SPI_CR0_BHT_8BIT          (1u << 13)  /* 8-bit APB xfer */
#define SPI_CR0_XFM_TR            (0x00u << 18) /* xmit+recv     */
#define SPI_CR0_XFM_TO            (0x01u << 18) /* xmit only     */
#define SPI_CR0_XFM_RO            (0x02u << 18) /* recv only     */
#define SPI_CR0_OPM_MASTER        (0x00u << 20)
#define SPI_CR0_OPM_SLAVE         (0x01u << 20)

/* SR fields (SDK rk3506.h SPI_SR_*) */

#define SPI_SR_BSF                (1u << 0)   /* bus busy          */
#define SPI_SR_TFF                (1u << 1)   /* TX FIFO full      */
#define SPI_SR_TFE                (1u << 2)   /* TX FIFO empty     */
#define SPI_SR_RFE                (1u << 3)   /* RX FIFO empty     */
#define SPI_SR_RFF                (1u << 4)   /* RX FIFO full      */

/* SER: one bit per slave select */

#define SPI_SER_SLAVE(n)          (1u << (n))

/* spiclk source after rk3506_spi1_clock_init() muxes CLK_SPI1 to
 * xin24m with /1, so the baud divider runs from 24 MHz.
 */

#define RK3506_SPI_SPICLK         24000000

/* Per-word wait timeout (us).  At 1 MHz a word takes ~8 us; 100 ms is
 * a generous bound that only trips when the bus is truly dead.
 */

#define RK3506_SPI_TIMEOUT_US     100000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_spi_dev_s
{
  struct spi_dev_s       dev;       /* Public NuttX SPI device */
  spinlock_t             lock;      /* Bus lock */
  uint32_t               base;      /* Register base address */
  int                    bus;       /* Bus number */
  uint32_t               freq;      /* Current clock rate (Hz) */
  uint8_t                mode;      /* Current SPI mode 0..3 */
  bool                   init;      /* Initialized? */
  irqstate_t             flags;     /* Saved IRQ flags for lock */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3506_spi_dev_s g_spi_dev[RK3506_SPI_NUM_BUS];

static const uint32_t g_spi_base[RK3506_SPI_NUM_BUS] =
{
  RK3506_SPI0_ADDR,
  RK3506_SPI1_ADDR
};

static const int g_spi_irq[RK3506_SPI_NUM_BUS] =
{
  RK3506_IRQ_SPI0,
  RK3506_IRQ_SPI1
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t rk3506_spi_getreg(struct rk3506_spi_dev_s *priv,
                                          uint32_t off);
static inline void     rk3506_spi_putreg(struct rk3506_spi_dev_s *priv,
                                          uint32_t off, uint32_t val);
static void            rk3506_spi_flush_rx(struct rk3506_spi_dev_s *priv);
static void            rk3506_spi_set_mode(struct rk3506_spi_dev_s *priv,
                                            uint8_t mode);
static void            rk3506_spi_set_freq(struct rk3506_spi_dev_s *priv,
                                            uint32_t freq);
static void            rk3506_spi_hw_init(struct rk3506_spi_dev_s *priv);

static int      rk3506_spi_lock(FAR struct spi_dev_s *dev, bool lock);
static void     rk3506_spi_select(FAR struct spi_dev_s *dev, uint32_t devid,
                                   bool selected);
static uint32_t rk3506_spi_setfrequency(FAR struct spi_dev_s *dev,
                                         uint32_t frequency);
static void     rk3506_spi_setmode(FAR struct spi_dev_s *dev,
                                    enum spi_mode_e mode);
static void     rk3506_spi_setbits(FAR struct spi_dev_s *dev, int nbits);
static uint8_t  rk3506_spi_status(FAR struct spi_dev_s *dev, uint32_t devid);
static uint32_t rk3506_spi_send(FAR struct spi_dev_s *dev, uint32_t wd);
#ifdef CONFIG_SPI_EXCHANGE
static void     rk3506_spi_exchange(FAR struct spi_dev_s *dev,
                                      const void *txbuffer, void *rxbuffer,
                                      size_t nwords);
#else
static void     rk3506_spi_sndblock(FAR struct spi_dev_s *dev,
                                      FAR const void *buffer, size_t nwords);
static void     rk3506_spi_recvblock(FAR struct spi_dev_s *dev,
                                      FAR void *buffer, size_t nwords);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rk3506_spi_getreg(struct rk3506_spi_dev_s *priv,
                                          uint32_t off)
{
  return getreg32(priv->base + off);
}

static inline void rk3506_spi_putreg(struct rk3506_spi_dev_s *priv,
                                      uint32_t off, uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3506_spi_flush_rx
 *
 * Description:
 *   Drain any stale words left in the RX FIFO.
 *
 ****************************************************************************/

static void rk3506_spi_flush_rx(struct rk3506_spi_dev_s *priv)
{
  while (rk3506_spi_getreg(priv, SPI_REG_RXFLR) > 0)
    {
      (void)rk3506_spi_getreg(priv, SPI_REG_RXDR);
    }
}

/****************************************************************************
 * Name: rk3506_spi_set_mode
 *
 * Description:
 *   Build CTRLR0 for 8-bit master mode with the requested CPOL/CPHA.
 *   Fixed fields mirror the SDK HAL defaults: big endian, MSB first,
 *   8-bit APB transform, 1 sclk SS setup, full-duplex (TR) transfer.
 *
 ****************************************************************************/

static void rk3506_spi_set_mode(struct rk3506_spi_dev_s *priv,
                                 uint8_t mode)
{
  uint32_t cr0 = SPI_CR0_DFS_8BIT | SPI_CR0_CSM_0 | SPI_CR0_SSD_ONE |
                 SPI_CR0_EM_BIG | SPI_CR0_FBM_MSB | SPI_CR0_BHT_8BIT |
                 SPI_CR0_XFM_TR | SPI_CR0_OPM_MASTER;

  if (mode & 0x01)  /* CPHA = 1 */
    {
      cr0 |= SPI_CR0_SCPH;
    }

  if (mode & 0x02)  /* CPOL = 1 */
    {
      cr0 |= SPI_CR0_SCPOL;
    }

  rk3506_spi_putreg(priv, SPI_REG_CTRLR0, cr0);
  priv->mode = mode;
}

/****************************************************************************
 * Name: rk3506_spi_set_freq
 *
 * Description:
 *   Program BAUDR.  On rk_spi the serial clock is spiclk / BAUDR with
 *   BAUDR an even number (SDK HAL: div = round_up(spiclk/speed), then
 *   forced even with (div + 1) & 0xfffe).
 *
 ****************************************************************************/

static void rk3506_spi_set_freq(struct rk3506_spi_dev_s *priv,
                                 uint32_t freq)
{
  uint32_t div;

  if (freq == 0)
    {
      freq = SPI_FREQ_DEFAULT;
    }

  if (freq > SPI_FREQ_MAX)
    {
      freq = SPI_FREQ_MAX;
    }

  div = (RK3506_SPI_SPICLK + freq - 1) / freq;  /* round up */
  div = (div + 1) & 0xfffe;                      /* force even */
  if (div < 2)
    {
      div = 2;
    }

  rk3506_spi_putreg(priv, SPI_REG_BAUDR, div);
  priv->freq = RK3506_SPI_SPICLK / div;
}

/****************************************************************************
 * Name: rk3506_spi_hw_init
 *
 * Description:
 *   Program controller defaults with the engine disabled, then enable.
 *
 ****************************************************************************/

static void rk3506_spi_hw_init(struct rk3506_spi_dev_s *priv)
{
  /* Disable while programming CTRLR0 / BAUDR */

  rk3506_spi_putreg(priv, SPI_REG_ENR, 0);

  /* Clear any leftover RX data */

  rk3506_spi_flush_rx(priv);

  /* Framing + clock */

  rk3506_spi_set_mode(priv, SPIDEV_MODE0);
  rk3506_spi_set_freq(priv, SPI_FREQ_DEFAULT);

  /* No slave select active yet */

  rk3506_spi_putreg(priv, SPI_REG_SER, 0);

  /* FIFO watermarks (polling does not rely on them, but set sane values) */

  rk3506_spi_putreg(priv, SPI_REG_TXFTLR, SPI_FIFO_DEPTH / 2 - 1);
  rk3506_spi_putreg(priv, SPI_REG_RXFTLR, SPI_FIFO_DEPTH / 2 - 1);

  /* No DMA, all interrupts masked (polling only) */

  rk3506_spi_putreg(priv, SPI_REG_DMACR, 0);
  rk3506_spi_putreg(priv, SPI_REG_IMR, 0);

  /* Enable */

  rk3506_spi_putreg(priv, SPI_REG_ENR, 1);
  priv->init = true;

  spiinfo("SPI%d: rk_spi initialized (base=0x%08lx, sclk=%lu Hz)\n",
          priv->bus, (unsigned long)priv->base,
          (unsigned long)priv->freq);
}

/****************************************************************************
 * spi_ops_s glue
 ****************************************************************************/

static int rk3506_spi_lock(FAR struct spi_dev_s *dev, bool lock)
{
  struct rk3506_spi_dev_s *priv = (struct rk3506_spi_dev_s *)dev;
  if (lock)
    {
      priv->flags = spin_lock_irqsave(&priv->lock);
    }
  else
    {
      spin_unlock_irqrestore(&priv->lock, priv->flags);
    }

  return 0;
}

static void rk3506_spi_select(FAR struct spi_dev_s *dev, uint32_t devid,
                                bool selected)
{
  struct rk3506_spi_dev_s *priv = (struct rk3506_spi_dev_s *)dev;
  uint32_t slave = devid & 0x1;
  uint32_t ser;

  ser = rk3506_spi_getreg(priv, SPI_REG_SER);
  if (selected)
    {
      ser |= SPI_SER_SLAVE(slave);
    }
  else
    {
      ser &= ~SPI_SER_SLAVE(slave);
    }

  rk3506_spi_putreg(priv, SPI_REG_SER, ser);
}

static uint32_t rk3506_spi_setfrequency(FAR struct spi_dev_s *dev,
                                         uint32_t frequency)
{
  struct rk3506_spi_dev_s *priv = (struct rk3506_spi_dev_s *)dev;
  uint32_t old = priv->freq;

  rk3506_spi_putreg(priv, SPI_REG_ENR, 0);
  rk3506_spi_set_freq(priv, frequency);
  rk3506_spi_putreg(priv, SPI_REG_ENR, 1);
  return old;
}

static void rk3506_spi_setmode(FAR struct spi_dev_s *dev,
                                enum spi_mode_e mode)
{
  struct rk3506_spi_dev_s *priv = (struct rk3506_spi_dev_s *)dev;

  rk3506_spi_putreg(priv, SPI_REG_ENR, 0);
  rk3506_spi_set_mode(priv, (uint8_t)mode);
  rk3506_spi_putreg(priv, SPI_REG_ENR, 1);
}

static void rk3506_spi_setbits(FAR struct spi_dev_s *dev, int nbits)
{
  /* Fixed at 8 bits per word */

  (void)dev;
  (void)nbits;
}

static uint8_t rk3506_spi_status(FAR struct spi_dev_s *dev, uint32_t devid)
{
  (void)dev;
  (void)devid;
  return 0;
}

/****************************************************************************
 * Name: rk3506_spi_wait_txdrain / rk3506_spi_wait_rxavail
 *
 * Description:
 *   Bounded polls on the FIFO level counters.  Return true on success,
 *   false on timeout (so a dead bus can never hang the system).
 *
 ****************************************************************************/

static bool rk3506_spi_wait_txroom(struct rk3506_spi_dev_s *priv)
{
  uint32_t timeout = RK3506_SPI_TIMEOUT_US;
  while (rk3506_spi_getreg(priv, SPI_REG_TXFLR) >= SPI_FIFO_DEPTH)
    {
      if (--timeout == 0)
        {
          return false;
        }

      up_udelay(1);
    }

  return true;
}

static bool rk3506_spi_wait_rxavail(struct rk3506_spi_dev_s *priv)
{
  uint32_t timeout = RK3506_SPI_TIMEOUT_US;
  while (rk3506_spi_getreg(priv, SPI_REG_RXFLR) == 0)
    {
      if (--timeout == 0)
        {
          return false;
        }

      up_udelay(1);
    }

  return true;
}

/****************************************************************************
 * Name: rk3506_spi_xfer_one
 *
 * Description:
 *   Shift one byte full-duplex.  On timeout the RX slot is filled with
 *   0xFF (the usual idle/mark value) so the caller fails cleanly.
 *
 ****************************************************************************/

static void rk3506_spi_xfer_one(struct rk3506_spi_dev_s *priv,
                                 uint8_t tx, uint8_t *rx)
{
  uint8_t val = 0xff;

  if (rk3506_spi_wait_txroom(priv))
    {
      rk3506_spi_putreg(priv, SPI_REG_TXDR, tx);
      if (rk3506_spi_wait_rxavail(priv))
        {
          val = (uint8_t)rk3506_spi_getreg(priv, SPI_REG_RXDR);
        }
    }

  if (rx)
    {
      *rx = val;
    }
}

#ifdef CONFIG_SPI_EXCHANGE
static void rk3506_spi_exchange(FAR struct spi_dev_s *dev,
                                 const void *txbuffer, void *rxbuffer,
                                 size_t nwords)
{
  struct rk3506_spi_dev_s *priv = (struct rk3506_spi_dev_s *)dev;
  FAR const uint8_t *tx = (FAR const uint8_t *)txbuffer;
  FAR uint8_t *rx = (FAR uint8_t *)rxbuffer;
  size_t i;

  if (txbuffer == NULL && rxbuffer == NULL)
    {
      return;
    }

  rk3506_spi_flush_rx(priv);

  for (i = 0; i < nwords; i++)
    {
      uint8_t t = tx ? tx[i] : 0xff;
      uint8_t r;

      rk3506_spi_xfer_one(priv, t, &r);
      if (rx)
        {
          rx[i] = r;
        }
    }

  /* Wait for the shifter to drain so CS does not cut the last bits */

  {
    uint32_t timeout = RK3506_SPI_TIMEOUT_US;
    while ((rk3506_spi_getreg(priv, SPI_REG_SR) &
            (SPI_SR_TFE | SPI_SR_BSF)) != SPI_SR_TFE)
      {
        if (--timeout == 0)
          {
            break;
          }

        up_udelay(1);
      }
  }
}
#else
static void rk3506_spi_sndblock(FAR struct spi_dev_s *dev,
                                 FAR const void *buffer, size_t nwords)
{
  struct rk3506_spi_dev_s *priv = (struct rk3506_spi_dev_s *)dev;
  FAR const uint8_t *tx = (FAR const uint8_t *)buffer;
  size_t i;

  rk3506_spi_flush_rx(priv);
  for (i = 0; i < nwords; i++)
    {
      rk3506_spi_xfer_one(priv, tx[i], NULL);
    }
}

static void rk3506_spi_recvblock(FAR struct spi_dev_s *dev,
                                 FAR void *buffer, size_t nwords)
{
  struct rk3506_spi_dev_s *priv = (struct rk3506_spi_dev_s *)dev;
  FAR uint8_t *rx = (FAR uint8_t *)buffer;
  size_t i;

  rk3506_spi_flush_rx(priv);
  for (i = 0; i < nwords; i++)
    {
      rk3506_spi_xfer_one(priv, 0xff, &rx[i]);
    }
}
#endif

static uint32_t rk3506_spi_send(FAR struct spi_dev_s *dev, uint32_t wd)
{
  struct rk3506_spi_dev_s *priv = (struct rk3506_spi_dev_s *)dev;
  uint8_t rx;

  rk3506_spi_xfer_one(priv, (uint8_t)wd, &rx);
  return (uint32_t)rx;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct spi_dev_s *rk3506_spibus_initialize(int bus)
{
  struct rk3506_spi_dev_s *priv;
  static bool inited = false;

  if (bus < 0 || bus >= RK3506_SPI_NUM_BUS)
    {
      _err("ERROR: invalid SPI bus %d\n", bus);
      return NULL;
    }

  priv = &g_spi_dev[bus];

  if (!inited)
    {
      for (int i = 0; i < RK3506_SPI_NUM_BUS; i++)
        {
          g_spi_dev[i].base  = g_spi_base[i];
          g_spi_dev[i].bus   = i;
          g_spi_dev[i].freq  = 0;
          g_spi_dev[i].mode  = SPIDEV_MODE0;
          g_spi_dev[i].init  = false;
          spin_lock_init(&g_spi_dev[i].lock);
        }

      inited = true;
    }

  if (priv->dev.ops == NULL)
    {
      static const struct spi_ops_s rk3506_spi_ops =
      {
        .lock         = rk3506_spi_lock,
        .select       = rk3506_spi_select,
        .setfrequency = rk3506_spi_setfrequency,
        .setmode      = rk3506_spi_setmode,
        .setbits      = rk3506_spi_setbits,
        .status       = rk3506_spi_status,
        .send         = rk3506_spi_send,
#ifdef CONFIG_SPI_EXCHANGE
        .exchange     = rk3506_spi_exchange,
#else
        .sndblock     = rk3506_spi_sndblock,
        .recvblock    = rk3506_spi_recvblock,
#endif
      };

      priv->dev.ops = &rk3506_spi_ops;
    }

  rk3506_spi_hw_init(priv);
  return &priv->dev;
}

void rk3506_spibus_uninitialize(int bus)
{
  struct rk3506_spi_dev_s *priv = &g_spi_dev[bus];
  rk3506_spi_putreg(priv, SPI_REG_ENR, 0);
  rk3506_spi_putreg(priv, SPI_REG_SER, 0);
  priv->init = false;
}
