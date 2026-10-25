/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_nv3007.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NV3007 2.79" SPI TFT panel (module T279VJ-C10-01) lower-half driver.
 *
 * The panel is a memory-less SPI LCD implementing the MIPI-DBI command
 * set plus a vendor register block.  This driver exposes the NuttX
 * lcd_dev_s interface; with CONFIG_LCD_FRAMEBUFFER the framework wraps it
 * as /dev/fb0 (RAM buffer + FBIO_UPDATE -> putarea), which the LVGL port
 * mmaps and renders into.
 *
 * Only SCK/MOSI go through the SPI0 controller RM_IO crossbar; CS, DC, RST
 * and the backlight are ordinary GPIO outputs.  J9 header, 3.3 V:
 *   J9-17 SCK  = GPIO0_A7 / RM_IO7  <- spi0_clk  (routing fn 82)
 *   J9-16 SDA  = GPIO0_A1 / RM_IO1  <- spi0_mosi (routing fn 83)
 *   J9-14 CS   = GPIO0_A3 / RM_IO3  (GPIO output, active low)
 *   J9-31 RST  = GPIO0_B0 / RM_IO8  (GPIO output)
 *   J9-30 DC   = GPIO0_B3 / RM_IO11 (GPIO output)
 *   J9-29 BL   = GPIO0_B4 / RM_IO12 (GPIO output, high = lit)
 *
 * The module has NO MISO line, so register read-back (and therefore the
 * usual "read the panel ID to prove the link" trick) is impossible;
 * nv_getrun() reports -ENOTSUP.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/video/fb.h>
#include <nuttx/signal.h>

#include "rk3506_spi.h"
#include "rk3506_gpio.h"
#include "rk3506_pinctrl.h"
#include "rk3506_register.h"
#include "hardware/rk3506_memorymap.h"

#include "hd_rk3506_nv3007.h"

#ifdef CONFIG_LCD_NV3007

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SPI0 clock/MOSI pads are routed to the controller through the RM_IO
 * matrix (rk3506_pinctrl_set_mux with the DTS function numbers 82/83).
 * CS is a plain software GPIO, so no csn0 routing is programmed -- but
 * SPI_SELECT() is still mandatory, see nv_bus_acquire() below.
 */

#define NV3007_PIN_SPI_CLK     7     /* GPIO0_A7 -> J9-17 (RM_IO, fn 82) */
#define NV3007_PIN_SPI_MOSI    1     /* GPIO0_A1 -> J9-16 (RM_IO, fn 83) */

/* CS/RST/DC/BL are driven through the shared GPIO back-end
 * (rk3506_gpio_config/write).  They live on GPIO0.
 */

#define NV_GPIN_CS             RK3506_GPIO_PIN(0, NV3007_PIN_CS)
#define NV_GPIN_RST            RK3506_GPIO_PIN(0, NV3007_PIN_RST)
#define NV_GPIN_DC             RK3506_GPIO_PIN(0, NV3007_PIN_DC)
#define NV_GPIN_BL             RK3506_GPIO_PIN(0, NV3007_PIN_BL)

/* MIPI-DBI commands */

#define NV_CMD_SWRESET        0x01
#define NV_CMD_SLPIN          0x10
#define NV_CMD_SLPOUT         0x11
#define NV_CMD_INVOFF         0x20
#define NV_CMD_INVON          0x21
#define NV_CMD_DISPOFF        0x28
#define NV_CMD_DISPON         0x29
#define NV_CMD_CASET          0x2a
#define NV_CMD_RASET          0x2b
#define NV_CMD_RAMWR          0x2c
#define NV_CMD_MADCTL         0x36
#define NV_CMD_COLMOD         0x3a

/* Marker in the init table: send p[0] as a data byte with no command
 * (the vendor demo emits a lone 0x17 data byte between F1 and F2).
 */

#define NV_DATA_ONLY          0x100u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct nv3007_dev_s
{
  struct lcd_dev_s  dev;       /* Must be first */
  FAR struct spi_dev_s *spi;
  uint8_t           power;     /* 0 = off, 1..CONFIG_LCD_MAXPOWER */
  bool              configured;/* SPI mode/bits/freq already applied  */
};

struct nv_init_cmd_s
{
  uint16_t cmd;                /* command, or NV_DATA_ONLY */
  uint8_t  n;                  /* number of parameter bytes */
  uint8_t  delay;              /* ms to wait after the command */
  uint8_t  p[4];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* g_runbuf[]  : the pinfo->buffer working line for the framebuffer layer.
 */

static uint8_t  g_runbuf[NV3007_XRES * (NV3007_BPP / 8)];

static struct nv3007_dev_s g_nv;

/* Vendor initialization sequence (from the T279VJ-C10-01 demo).
 *
 * ORDER MATTERS and the vendor order is the one that works: the stream
 * opens the vendor register page (0xff 0xa5) as the FIRST command right
 * after the hardware reset, while the panel is still in sleep-in.  Sending
 * Sleep Out (0x11) first makes the ~110 GOA/power register writes get
 * dropped, so 0x29 "succeeds" but the panel shows nothing.  Do not
 * "improve" this by hoisting 0x11 / 0x36 / 0x3a to the top.
 */

static const struct nv_init_cmd_s g_init[] =
{
  { 0xff, 1,   0, { 0xa5 } },    /* open vendor register page (FIRST) */
  { 0x9a, 1,   0, { 0x08 } },
  { 0x9b, 1,   0, { 0x08 } },
  { 0x9c, 1,   0, { 0xb0 } },
  { 0x9d, 1,   0, { 0x16 } },
  { 0x9e, 1,   0, { 0xc4 } },
  { 0x8f, 2,   0, { 0x55, 0x04 } },
  { 0x84, 1,   0, { 0x90 } },
  { 0x83, 1,   0, { 0x7b } },
  { 0x85, 1,   0, { 0x33 } },
  { 0x60, 1,   0, { 0x00 } },
  { 0x70, 1,   0, { 0x00 } },
  { 0x61, 1,   0, { 0x02 } },
  { 0x71, 1,   0, { 0x02 } },
  { 0x62, 1,   0, { 0x04 } },
  { 0x72, 1,   0, { 0x04 } },
  { 0x6c, 1,   0, { 0x29 } },
  { 0x7c, 1,   0, { 0x29 } },
  { 0x6d, 1,   0, { 0x31 } },
  { 0x7d, 1,   0, { 0x31 } },
  { 0x6e, 1,   0, { 0x0f } },
  { 0x7e, 1,   0, { 0x0f } },
  { 0x66, 1,   0, { 0x21 } },
  { 0x76, 1,   0, { 0x21 } },
  { 0x68, 1,   0, { 0x3a } },
  { 0x78, 1,   0, { 0x3a } },
  { 0x63, 1,   0, { 0x07 } },
  { 0x73, 1,   0, { 0x07 } },
  { 0x64, 1,   0, { 0x05 } },
  { 0x74, 1,   0, { 0x05 } },
  { 0x65, 1,   0, { 0x02 } },
  { 0x75, 1,   0, { 0x02 } },
  { 0x67, 1,   0, { 0x23 } },
  { 0x77, 1,   0, { 0x23 } },
  { 0x69, 1,   0, { 0x08 } },
  { 0x79, 1,   0, { 0x08 } },
  { 0x6a, 1,   0, { 0x13 } },
  { 0x7a, 1,   0, { 0x13 } },
  { 0x6b, 1,   0, { 0x13 } },
  { 0x7b, 1,   0, { 0x13 } },
  { 0x6f, 1,   0, { 0x00 } },
  { 0x7f, 1,   0, { 0x00 } },
  { 0x50, 1,   0, { 0x00 } },
  { 0x52, 1,   0, { 0xd6 } },
  { 0x53, 1,   0, { 0x08 } },
  { 0x54, 1,   0, { 0x08 } },
  { 0x55, 1,   0, { 0x1e } },
  { 0x56, 1,   0, { 0x1c } },
  { 0xa0, 3,   0, { 0x2b, 0x24, 0x00 } },
  { 0xa1, 1,   0, { 0x87 } },
  { 0xa2, 1,   0, { 0x86 } },
  { 0xa5, 1,   0, { 0x00 } },
  { 0xa6, 1,   0, { 0x00 } },
  { 0xa7, 1,   0, { 0x00 } },
  { 0xa8, 1,   0, { 0x36 } },
  { 0xa9, 1,   0, { 0x7e } },
  { 0xaa, 1,   0, { 0x7e } },
  { 0xb9, 1,   0, { 0x85 } },
  { 0xba, 1,   0, { 0x84 } },
  { 0xbb, 1,   0, { 0x83 } },
  { 0xbc, 1,   0, { 0x82 } },
  { 0xbd, 1,   0, { 0x81 } },
  { 0xbe, 1,   0, { 0x80 } },
  { 0xbf, 1,   0, { 0x01 } },
  { 0xc0, 1,   0, { 0x02 } },
  { 0xc1, 1,   0, { 0x00 } },
  { 0xc2, 1,   0, { 0x00 } },
  { 0xc3, 1,   0, { 0x00 } },
  { 0xc4, 1,   0, { 0x33 } },
  { 0xc5, 1,   0, { 0x7e } },
  { 0xc6, 1,   0, { 0x7e } },
  { 0xc8, 2,   0, { 0x33, 0x33 } },
  { 0xc9, 1,   0, { 0x68 } },
  { 0xca, 1,   0, { 0x69 } },
  { 0xcb, 1,   0, { 0x6a } },
  { 0xcc, 1,   0, { 0x6b } },
  { 0xcd, 2,   0, { 0x33, 0x33 } },
  { 0xce, 1,   0, { 0x6c } },
  { 0xcf, 1,   0, { 0x6d } },
  { 0xd0, 1,   0, { 0x6e } },
  { 0xd1, 1,   0, { 0x6f } },
  { 0xab, 2,   0, { 0x03, 0x67 } },
  { 0xac, 2,   0, { 0x03, 0x6b } },
  { 0xad, 2,   0, { 0x03, 0x68 } },
  { 0xae, 2,   0, { 0x03, 0x6c } },
  { 0xb3, 1,   0, { 0x00 } },
  { 0xb4, 1,   0, { 0x00 } },
  { 0xb5, 1,   0, { 0x00 } },
  { 0xb6, 1,   0, { 0x32 } },
  { 0xb7, 1,   0, { 0x7e } },
  { 0xb8, 1,   0, { 0x7e } },
  { 0xe0, 1,   0, { 0x00 } },
  { 0xe1, 2,   0, { 0x03, 0x0f } },
  { 0xe2, 1,   0, { 0x04 } },
  { 0xe3, 1,   0, { 0x01 } },
  { 0xe4, 1,   0, { 0x0e } },
  { 0xe5, 1,   0, { 0x01 } },
  { 0xe6, 1,   0, { 0x19 } },
  { 0xe7, 1,   0, { 0x10 } },
  { 0xe8, 1,   0, { 0x10 } },
  { 0xea, 1,   0, { 0x12 } },
  { 0xeb, 1,   0, { 0xd0 } },
  { 0xec, 1,   0, { 0x04 } },
  { 0xed, 1,   0, { 0x07 } },
  { 0xee, 1,   0, { 0x07 } },
  { 0xef, 1,   0, { 0x09 } },
  { 0xf0, 1,   0, { 0xd0 } },
  { 0xf1, 1,   0, { 0x0e } },
  { NV_DATA_ONLY, 1, 0, { 0x17 } },  /* vendor stream: lone data byte */
  { 0xf2, 4,   0, { 0x2c, 0x1b, 0x0b, 0x20 } },
  { 0xe9, 1,   0, { 0x29 } },
  { 0xec, 1,   0, { 0x04 } },
  { 0x35, 1,   0, { 0x00 } },
  { 0x44, 2,   0, { 0x00, 0x10 } },
  { 0x46, 1,   0, { 0x10 } },
  { 0xff, 1,   0, { 0x00 } },    /* close vendor register page */
  { 0x3a, 1,   0, { 0x05 } },    /* COLMOD: 16-bit RGB565 (vendor order) */

  /* Sleep Out + 220 ms, then Display ON + 200 ms.  Both waits are in the
   * vendor/ESP-IDF timing budget (the controller needs the second one to
   * bring up the GOA before it will show the first frame).
   */

  { 0x11, 0, 220, { 0 } },       /* Sleep Out + 220 ms (vendor timing) */
  { 0x29, 0, 200, { 0 } },       /* Display ON + 200 ms               */
};

/****************************************************************************
 * Private Functions: pad/GPIO/SPI low level
 ****************************************************************************/

/* CS and DC are GPIO lines shared with the SPI engine, so every switch
 * carries an explicit setup/hold delay.  Without this, DC can change
 * mid-byte and the controller latches the tail of a parameter stream as a
 * command.
 */

static void nv_cs_select(bool selected)
{
  if (selected)
    {
      rk3506_gpio_write(NV_GPIN_CS, false);      /* CS active low */
      up_udelay(NV3007_CS_SETUP_US);
    }
  else
    {
      up_udelay(NV3007_CS_HOLD_US);
      rk3506_gpio_write(NV_GPIN_CS, true);
    }
}

static void nv_dc_set(bool data)
{
  rk3506_gpio_write(NV_GPIN_DC, data);
  up_udelay(NV3007_DC_SETUP_US);
}

/* Route SCK/MOSI through the RM_IO crossbar to SPI0 and set up the GPIO
 * control lines.  The backlight pad is configured as a GPIO output held
 * low here (dark), so nothing lights up before setpower() runs.
 */

static void nv_pinctrl(void)
{
  rk3506_pinctrl_set_mux(0, NV3007_PIN_SPI_CLK,  82u);  /* A7 CLK  */
  rk3506_pinctrl_set_mux(0, NV3007_PIN_SPI_MOSI, 83u);  /* A1 MOSI */

  rk3506_gpio_config(NV_GPIN_CS,  RK3506_GPIO_OUTPUT | RK3506_GPIO_VALUE_HIGH);
  rk3506_gpio_config(NV_GPIN_RST, RK3506_GPIO_OUTPUT | RK3506_GPIO_VALUE_HIGH);
  rk3506_gpio_config(NV_GPIN_DC,  RK3506_GPIO_OUTPUT | RK3506_GPIO_VALUE_HIGH);
  rk3506_gpio_config(NV_GPIN_BL,  RK3506_GPIO_OUTPUT | RK3506_GPIO_VALUE_LOW);

  /* DIAG: verify the RMIO matrix actually connects GPIO0_A1/A7 to the
   * pads.  If the RMIO write landed, the pad should follow the GPIO
   * controller.  We toggle the GPIO and read back the pad level.
   */

  {
    /* A1 and A7 are RMIO pins; we cannot drive them as GPIO without
     * breaking the SPI routing.  Instead we verify the RMIO register
     * values and the IOMUX nibble.
     */

    int mux_a1 = rk3506_pinctrl_get_mux(0, NV3007_PIN_SPI_MOSI);
    int mux_a7 = rk3506_pinctrl_get_mux(0, NV3007_PIN_SPI_CLK);

    _err("NV3007 RMIO: A1 mux=%d (want 7), A7 mux=%d (want 7)\n",
         mux_a1, mux_a7);

    /* Read the RMIO channel registers directly */

    uint32_t sel_a1 = getreg32(RK3506_GRF_PMU_ADDR + 0x84);
    uint32_t sel_a7 = getreg32(RK3506_GRF_PMU_ADDR + 0x9c);

    _err("NV3007 RMIO: ch1(MOSI) SEL=0x%02x (want 68), "
         "ch7(CLK) SEL=0x%02x (want 67)\n",
         sel_a1 & 0x7f, sel_a7 & 0x7f);
  }
}

/* Take the bus and gate the engine on.
 *
 * SPI_SELECT is REQUIRED even though the panel CS is a plain GPIO: on the
 * Rockchip DesignWare SPI block the controller only generates SCLK for an
 * enabled slave-select (SER != 0).  With SER == 0 bytes land in the TX
 * FIFO but are never clocked out, which looks exactly like a dead panel
 * with the backlight on.  The hardware CSN0 pad is not routed, so the SER
 * bit only gates the engine; the panel's real CS is the GPIO above.
 *
 * Mode/bits/frequency are applied once, not on every transaction: some
 * dw_spi lower-halves re-program the controller in setbits()/setmode(),
 * and doing that between CS assertions can glitch SCLK.
 */

static void nv_bus_acquire(void)
{
  SPI_LOCK(g_nv.spi, true);

  if (!g_nv.configured)
    {
      SPI_SETMODE(g_nv.spi, NV3007_SPI_MODE);
      SPI_SETBITS(g_nv.spi, 8);
      SPI_SETFREQUENCY(g_nv.spi, NV3007_SPI_FREQ);
      g_nv.configured = true;
    }

  SPI_SELECT(g_nv.spi, SPIDEV_DISPLAY(0), true);
}

static void nv_bus_release(void)
{
  nv_cs_select(false);
  SPI_SELECT(g_nv.spi, SPIDEV_DISPLAY(0), false);
  SPI_LOCK(g_nv.spi, false);
}

/* Every byte goes out through SPI_EXCHANGE with a NULL receive buffer,
 * which blocks until the last byte has actually been shifted out.
 * SPI_SNDBLOCK on the DesignWare core is allowed to return as soon as the
 * words are queued, which drops the CS/DC change into the middle of a
 * transaction and truncates commands.
 */

static void nv_spi_write(FAR const void *tx, size_t nbytes)
{
  SPI_EXCHANGE(g_nv.spi, tx, NULL, nbytes);
}

/* One command plus its parameters inside a SINGLE CS window.
 *
 * The NV3007 resets its parameter index on CS-high, so a command and its
 * parameters must share one window -- splitting them silently drops every
 * parameter.  NV_DATA_ONLY emits a bare data byte with no command.
 */

static void nv_command(uint16_t cmd, FAR const uint8_t *p, uint8_t n)
{
  uint8_t i;

  nv_bus_acquire();

  if (cmd != NV_DATA_ONLY)
    {
      uint8_t c = (uint8_t)cmd;

      nv_cs_select(true);
      nv_dc_set(false);                 /* command phase */
      nv_spi_write(&c, 1);
      nv_cs_select(false);
    }

  for (i = 0; i < n; i++)
    {
      nv_cs_select(true);
      nv_dc_set(true);                  /* data phase */
      nv_spi_write(&p[i], 1);
      nv_cs_select(false);
    }

  nv_bus_release();
}

/* CASET/RASET/RAMWR window.  Runs under one CS assertion, ending with CS
 * held low and DC high so the pixel bytes that follow stream as one
 * continuous RAMWR burst.  The caller must call nv_bus_release().
 */

static void nv_stream_begin(fb_coord_t col_start, fb_coord_t col_end,
                            fb_coord_t row_start, fb_coord_t row_end)
{
  uint8_t col[4];
  uint8_t row[4];
  uint8_t c;
  uint8_t i;

  col_start += NV3007_XOFFSET;
  col_end   += NV3007_XOFFSET;

  col[0] = (uint8_t)(col_start >> 8);
  col[1] = (uint8_t)(col_start);
  col[2] = (uint8_t)(col_end >> 8);
  col[3] = (uint8_t)(col_end);

  row[0] = (uint8_t)(row_start >> 8);
  row[1] = (uint8_t)(row_start);
  row[2] = (uint8_t)(row_end >> 8);
  row[3] = (uint8_t)(row_end);

  nv_bus_acquire();

  /* CASET: cmd window, then 4 data windows */

  nv_cs_select(true);
  nv_dc_set(false);
  c = NV_CMD_CASET;
  nv_spi_write(&c, 1);
  nv_cs_select(false);

  for (i = 0; i < 4; i++)
    {
      nv_cs_select(true);
      nv_dc_set(true);
      nv_spi_write(&col[i], 1);
      nv_cs_select(false);
    }

  /* RASET: cmd window, then 4 data windows */

  nv_cs_select(true);
  nv_dc_set(false);
  c = NV_CMD_RASET;
  nv_spi_write(&c, 1);
  nv_cs_select(false);

  for (i = 0; i < 4; i++)
    {
      nv_cs_select(true);
      nv_dc_set(true);
      nv_spi_write(&row[i], 1);
      nv_cs_select(false);
    }

  /* RAMWR: cmd window only; pixel data follows in nv_send_pixels() */

  nv_cs_select(true);
  nv_dc_set(false);
  c = NV_CMD_RAMWR;
  nv_spi_write(&c, 1);
  nv_cs_select(false);
}

/* Push one row.  src is the framebuffer (RGB565 little-endian in memory);
 * the panel wants high byte first, so bytes are swapped into g_xlate[]
 * first.  g_xlate[] is private to the driver, never the caller's buffer.
 */

static void nv_send_pixels(FAR const uint8_t *src, unsigned int npixels)
{
  FAR const uint16_t *in = (FAR const uint16_t *)src;
  unsigned int i;

  if (npixels > NV3007_XRES)
    {
      npixels = NV3007_XRES;
    }

  /* Vendor demo sends every pixel byte in its own CS window (DC high). */

  for (i = 0; i < npixels; i++)
    {
      uint8_t hi = (uint8_t)(in[i] >> 8);
      uint8_t lo = (uint8_t)(in[i]);

      nv_cs_select(true);
      nv_dc_set(true);
      nv_spi_write(&hi, 1);
      nv_cs_select(false);

      nv_cs_select(true);
      nv_dc_set(true);
      nv_spi_write(&lo, 1);
      nv_cs_select(false);
    }
}

static void nv_hw_reset(void)
{
  /* The controller needs VCC stable and RES released before any command.
   * Idle-high first, then a long low pulse, then the 120 ms recovery the
   * vendor demo uses.
   */

  rk3506_gpio_write(NV_GPIN_RST, true);
  nxsig_usleep(10 * 1000);
  rk3506_gpio_write(NV_GPIN_RST, false);
  nxsig_usleep(100 * 1000);
  rk3506_gpio_write(NV_GPIN_RST, true);
  nxsig_usleep(120 * 1000);
}

/* Backlight: plain GPIO, level 0 = off, non-zero = on.  The pad is
 * configured as a GPIO output (rk3506_gpio_config() also sets the IOMUX
 * nibble back to function 0) so no PWM lower-half is involved.
 */

static void nv_backlight(uint8_t level)
{
  rk3506_gpio_config(NV_GPIN_BL, (level != 0) ?
                     (RK3506_GPIO_OUTPUT | RK3506_GPIO_VALUE_HIGH) :
                     (RK3506_GPIO_OUTPUT | RK3506_GPIO_VALUE_LOW));
}

/****************************************************************************
 * lcd_dev_s callbacks
 ****************************************************************************/

static int nv_putrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                     fb_coord_t col, FAR const uint8_t *buffer,
                     size_t npixels);
static int nv_putarea(FAR struct lcd_dev_s *dev,
                      fb_coord_t row_start, fb_coord_t row_end,
                      fb_coord_t col_start, fb_coord_t col_end,
                      FAR const uint8_t *buffer, fb_coord_t stride);
static int nv_getrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                     fb_coord_t col, FAR uint8_t *buffer, size_t npixels);

static int nv_getvideoinfo(FAR struct lcd_dev_s *dev,
                           FAR struct fb_videoinfo_s *vinfo)
{
  vinfo->fmt     = FB_FMT_RGB16_565;
  vinfo->xres    = NV3007_XRES;
  vinfo->yres    = NV3007_YRES;
  vinfo->nplanes = 1;
  return OK;
}

static int nv_getplaneinfo(FAR struct lcd_dev_s *dev, unsigned int planeno,
                           FAR struct lcd_planeinfo_s *pinfo)
{
  if (planeno != 0)
    {
      return -EINVAL;
    }

  memset(pinfo, 0, sizeof(*pinfo));
  pinfo->putrun  = nv_putrun;
  pinfo->putarea = nv_putarea;
  pinfo->getrun  = nv_getrun;
  pinfo->buffer  = g_runbuf;
  pinfo->bpp     = NV3007_BPP;
  pinfo->dev     = dev;
  return OK;
}

static int nv_putrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                     fb_coord_t col, FAR const uint8_t *buffer,
                     size_t npixels)
{
  if ((size_t)col + npixels > NV3007_XRES)
    {
      npixels = NV3007_XRES - col;
    }

  nv_stream_begin(col, col + (fb_coord_t)npixels - 1, row, row);
  nv_send_pixels(buffer, (unsigned int)npixels);
  nv_bus_release();
  return OK;
}

static int nv_putarea(FAR struct lcd_dev_s *dev,
                      fb_coord_t row_start, fb_coord_t row_end,
                      fb_coord_t col_start, fb_coord_t col_end,
                      FAR const uint8_t *buffer, fb_coord_t stride)
{
  unsigned int cols = (unsigned int)(col_end - col_start + 1);
  fb_coord_t row;

  if (cols > NV3007_XRES)
    {
      cols = NV3007_XRES;
    }

  if (row_end >= NV3007_YRES)
    {
      row_end = NV3007_YRES - 1;
    }

  nv_stream_begin(col_start, col_start + (fb_coord_t)cols - 1,
                  row_start, row_end);

  for (row = row_start; row <= row_end; row++)
    {
      nv_send_pixels(buffer, cols);
      buffer += stride;
    }

  nv_bus_release();
  return OK;
}

static int nv_getrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                     fb_coord_t col, FAR uint8_t *buffer, size_t npixels)
{
  /* The module has no MISO wired; framebuffer readback is unsupported. */

  return -ENOTSUP;
}

static int nv_getpower(FAR struct lcd_dev_s *dev)
{
  return g_nv.power;
}

static int nv_setpower(FAR struct lcd_dev_s *dev, int power)
{
  if (power < 0)
    {
      power = 0;
    }
  else if (power > CONFIG_LCD_MAXPOWER)
    {
      power = CONFIG_LCD_MAXPOWER;
    }

  g_nv.power = (uint8_t)power;
  nv_backlight(g_nv.power);
  return OK;
}

static int nv_getcontrast(FAR struct lcd_dev_s *dev)
{
  return 0;
}

static int nv_setcontrast(FAR struct lcd_dev_s *dev, unsigned int contrast)
{
  return OK;
}

/****************************************************************************
 * Private Data (ops)
 ****************************************************************************/

static const struct lcd_dev_s g_nv_ops =
{
  .getvideoinfo = nv_getvideoinfo,
  .getplaneinfo = nv_getplaneinfo,
  .getpower     = nv_getpower,
  .setpower     = nv_setpower,
  .getcontrast  = nv_getcontrast,
  .setcontrast  = nv_setcontrast,
};

static void nv_run_table(FAR const struct nv_init_cmd_s *t, unsigned int n)
{
  unsigned int i;

  for (i = 0; i < n; i++)
    {
      nv_command(t[i].cmd, t[i].p, t[i].n);

      if (t[i].delay > 0)
        {
          nxsig_usleep((useconds_t)t[i].delay * 1000);
        }
    }
}

/****************************************************************************
 * Public Functions: NuttX lcd_framebuffer board hooks
 ****************************************************************************/

int board_lcd_initialize(void)
{
  static bool initialized;

  if (initialized)
    {
      return OK;
    }

  memset(&g_nv, 0, sizeof(g_nv));
  g_nv.dev   = g_nv_ops;
  g_nv.power = 0;

  nv_pinctrl();

  g_nv.spi = rk3506_spibus_initialize(NV3007_SPI_BUS);
  if (g_nv.spi == NULL)
    {
      _err("NV3007: SPI%d init failed\n", NV3007_SPI_BUS);
      return -ENODEV;
    }

  /* Power-up settle before the first edge on SCK. */

  nxsig_usleep(40 * 1000);

  nv_hw_reset();
  nv_run_table(g_init, sizeof(g_init) / sizeof(g_init[0]));

  nv_setpower(&g_nv.dev, CONFIG_LCD_MAXPOWER);

  initialized = true;

  _info("NV3007: %dx%d RGB565 on SPI0 J9 (CLK17/MOSI16/CS14, RST31/DC30/BL29)"
        ", mode %d, %lu Hz, /dev/fb0\n",
        NV3007_XRES, NV3007_YRES, (int)NV3007_SPI_MODE,
        (unsigned long)NV3007_SPI_FREQ);
  return OK;
}

FAR struct lcd_dev_s *board_lcd_getdev(int lcddev)
{
  if (lcddev != 0)
    {
      return NULL;
    }

  return &g_nv.dev;
}

void board_lcd_uninitialize(void)
{
  nv_setpower(&g_nv.dev, 0);
}

#endif /* CONFIG_LCD_NV3007 */
