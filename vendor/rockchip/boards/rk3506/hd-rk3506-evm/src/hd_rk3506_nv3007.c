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
 * as /dev/fb0 (RAM buffer + FBIO_UPDATE -> putarea), which the existing
 * LVGL port mmaps and renders into.
 *
 * Only SCK/MOSI go through the SPI0 controller RM_IO crossbar; CS, DC
 * and RST are ordinary GPIO outputs toggled exactly as in the proven
 * vendor/Arduino bit-bang demo (per-byte software CS).  J9 header, 3.3 V:
 *   J9-17 SCK  = GPIO0_A7 / RM_IO7 <- spi0_clk  (routing fn 82)
 *   J9-16 SDA  = GPIO0_A1 / RM_IO1 <- spi0_mosi (routing fn 83)
 *   J9-14 CS   = GPIO0_A3 / RM_IO3  (GPIO output, active low, per-byte)
 *   J9-31 RST  = GPIO0_B0 / RM_IO8  (GPIO output, default SAI1_MCLK)
 *   J9-30 DC   = GPIO0_B3 / RM_IO11 (GPIO output, default SAI1_SDI)
 *   J9-29 BL   = GPIO0_B4 / RM_IO12 <- PWM0 ch2 (rk3506_pwm, dimming)
 *
 * The initialization table and the visible window (142 columns x 428 rows,
 * 12-column GRAM x-offset, RGB565 high-byte first) are transcribed
 * verbatim from the vendor demo 2.79TFT-T279VJ-C10-01-SPI-NV3007.ino.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <debug.h>

#include <nuttx/board.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/video/fb.h>
#include <nuttx/timers/pwm.h>
#include <nuttx/signal.h>

#include "rk3506_spi.h"
#include "rk3506_pwm.h"
#include "rk3506_gpio.h"
#include "rk3506_pinctrl.h"

#include "hd_rk3506_nv3007.h"

#ifdef CONFIG_LCD_NV3007

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SPI0 clock/MOSI pads are routed to the controller through the RM_IO
 * matrix (rk3506_pinctrl_set_mux with the DTS function numbers 82/83);
 * CS is a plain software GPIO (the proven vendor/Arduino wiring), so no
 * csn0 routing is programmed.  SPI CLK = GPIO0_A7 (J9-17), MOSI =
 * GPIO0_A1 (J9-16).
 */

#define NV3007_PIN_SPI_CLK     7     /* GPIO0_A7 -> J9-17 (RM_IO, fn 82) */
#define NV3007_PIN_SPI_MOSI    1     /* GPIO0_A1 -> J9-16 (RM_IO, fn 83) */

/* CS/RST/DC are driven through the shared GPIO back-end
 * (rk3506_gpio_config/write).  They live on GPIO0; the global pin
 * numbers below fold in the bank.
 */

#define NV_GPIN_CS             RK3506_GPIO_PIN(0, NV3007_PIN_CS)
#define NV_GPIN_RST            RK3506_GPIO_PIN(0, NV3007_PIN_RST)
#define NV_GPIN_DC             RK3506_GPIO_PIN(0, NV3007_PIN_DC)

/* MIPI-DBI commands used outside the init table */

#define NV_CMD_CASET         0x2a
#define NV_CMD_RASET         0x2b
#define NV_CMD_RAMWR         0x2c

/* Marker in the init table: send p[0] as a data byte with no command
 * (the vendor demo emits a lone 0x17 data byte between F1 and F2).
 */

#define NV_DATA_ONLY         0x100u

/* Backlight PWM */

#define NV_BL_FREQUENCY      40000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct nv3007_dev_s
{
  struct lcd_dev_s  dev;       /* Must be first */
  FAR struct spi_dev_s *spi;
  uint8_t           power;     /* 0 = off, 1..CONFIG_LCD_MAXPOWER */
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

/* One visible line, RGB565 -> panel byte order (high byte first) */

static uint8_t g_linebuf[NV3007_XRES * (NV3007_BPP / 8)];

static struct nv3007_dev_s g_nv;

/* Vendor initialization sequence, verbatim from the .ino TFT_Init(). */

static const struct nv_init_cmd_s g_init[] =
{
  { 0xff, 1,   0, { 0xa5 } },
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
  { 0xc8, 1,   0, { 0x33 } },
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
  { NV_DATA_ONLY, 1, 0, { 0x17 } },
  { 0xf2, 4,   0, { 0x2c, 0x1b, 0x0b, 0x20 } },
  { 0xe9, 1,   0, { 0x29 } },
  { 0xec, 1,   0, { 0x04 } },
  { 0x35, 1,   0, { 0x00 } },
  { 0x44, 2,   0, { 0x00, 0x10 } },
  { 0x46, 1,   0, { 0x10 } },
  { 0xff, 1,   0, { 0x00 } },
  { 0x3a, 1,   0, { 0x05 } },    /* COLMOD: 16-bit RGB565 */
  { 0x11, 0, 220, { 0 } },       /* Sleep Out + 220 ms */
  { 0x29, 0,   0, { 0 } },       /* Display ON */
};

/****************************************************************************
 * Private Functions: pad/GPIO/SPI low level
 ****************************************************************************/

static void nv_dc(bool data)
{
  rk3506_gpio_write(NV_GPIN_DC, data);
}

static void nv_cs(bool selected)
{
  rk3506_gpio_write(NV_GPIN_CS, !selected);     /* CS active low */
}

/* Route only SCK/MOSI through the SPI0 crossbar; make CS/RST/DC plain
 * GPIO outputs (software CS, exactly like the vendor/Arduino wiring).
 * All pin muxing / RM_IO routing / GPIO direction+level now go through
 * the unified chip back-ends (rk3506_pinctrl / rk3506_gpio) instead of
 * local register pokes.
 */

static void nv_pinctrl(void)
{
  /* RM_IO crossbar: SPI0 CLK->A7 (J9-17) func 82, MOSI->A1 (J9-16)
   * func 83.  rk3506_pinctrl_set_mux() sees func > 15, programs the
   * RM_IO channel with (func - 15) and forces the IOMUX nibble to 7.
   */

  rk3506_pinctrl_set_mux(0, NV3007_PIN_SPI_CLK,  82u);  /* A7 CLK  */
  rk3506_pinctrl_set_mux(0, NV3007_PIN_SPI_MOSI, 83u);  /* A1 MOSI */

  /* CS/RST/DC as plain GPIO outputs, idle high (CS deselected, RST/DC
   * inactive).  rk3506_gpio_config() muxes each pad to GPIO function 0,
   * ungates the GPIO0 clock and sets direction + initial level.
   */

  rk3506_gpio_config(NV_GPIN_CS,  RK3506_GPIO_OUTPUT | RK3506_GPIO_VALUE_HIGH);
  rk3506_gpio_config(NV_GPIN_RST, RK3506_GPIO_OUTPUT | RK3506_GPIO_VALUE_HIGH);
  rk3506_gpio_config(NV_GPIN_DC,  RK3506_GPIO_OUTPUT | RK3506_GPIO_VALUE_HIGH);
}

/* The vendor demo toggles CS for EVERY byte (CS low around the single
 * command byte, then CS low around each parameter byte).  The NV3007 does
 * not latch a multi-byte group under one CS assertion, so reproduce that
 * exactly.  Bus lock/mode/frequency are taken once; only SER (CS) and DC
 * toggle per byte.
 */

static void nv_select(void)
{
  SPI_LOCK(g_nv.spi, true);
  SPI_SETMODE(g_nv.spi, NV3007_SPI_MODE);
  SPI_SETBITS(g_nv.spi, 8);
  SPI_SETFREQUENCY(g_nv.spi, NV3007_SPI_FREQ);
}

static void nv_deselect(void)
{
  nv_cs(false);
  SPI_LOCK(g_nv.spi, false);
}

/* One byte with its own CS pulse, DC selects command(0)/data(1). */

static void nv_xfer_byte(uint8_t v, bool data)
{
  nv_cs(true);
  nv_dc(data);
  SPI_SEND(g_nv.spi, (uint16_t)v);
  nv_cs(false);
}

/* One command plus its parameters, each byte in its own CS assertion,
 * matching TFT_SEND_CMD / TFT_SEND_DATA exactly.
 */

static void nv_command(uint16_t cmd, FAR const uint8_t *p, uint8_t n)
{
  uint8_t k;

  nv_select();

  if (cmd == NV_DATA_ONLY)
    {
      nv_xfer_byte(p[0], true);
    }
  else
    {
      nv_xfer_byte((uint8_t)cmd, false);
      for (k = 0; k < n; k++)
        {
          nv_xfer_byte(p[k], true);
        }
    }

  nv_deselect();
}

/* CASET/RASET/RAMWR window, every byte with its own CS pulse (verbatim
 * TFT_SET_ADD).  Ends with CS held low and DC high so the following pixel
 * bytes stream as one RAMWR burst.
 */

static void nv_stream_begin(fb_coord_t c0, fb_coord_t c1,
                            fb_coord_t r0, fb_coord_t r1)
{
  c0 += NV3007_XOFFSET;
  c1 += NV3007_XOFFSET;

  nv_select();

  nv_xfer_byte(NV_CMD_CASET, false);
  nv_xfer_byte((uint8_t)(c0 >> 8), true);
  nv_xfer_byte((uint8_t)(c0), true);
  nv_xfer_byte((uint8_t)(c1 >> 8), true);
  nv_xfer_byte((uint8_t)(c1), true);

  nv_xfer_byte(NV_CMD_RASET, false);
  nv_xfer_byte((uint8_t)(r0 >> 8), true);
  nv_xfer_byte((uint8_t)(r0), true);
  nv_xfer_byte((uint8_t)(r1 >> 8), true);
  nv_xfer_byte((uint8_t)(r1), true);

  nv_xfer_byte(NV_CMD_RAMWR, false);

  /* Hold CS low / DC high for the continuous pixel burst. */

  nv_cs(true);
  nv_dc(true);
}

static void nv_send_pixels(FAR const uint8_t *src, unsigned int npixels)
{
  FAR const uint16_t *in = (FAR const uint16_t *)src;
  unsigned int i;

  for (i = 0; i < npixels; i++)
    {
      uint16_t color = in[i];
      g_linebuf[i * 2]     = (uint8_t)(color >> 8);
      g_linebuf[i * 2 + 1] = (uint8_t)(color);
    }

  SPI_SNDBLOCK(g_nv.spi, g_linebuf, npixels * 2);
}

static void nv_hw_reset(void)
{
  rk3506_gpio_write(NV_GPIN_RST, false);
  nxsig_usleep(100 * 1000);
  rk3506_gpio_write(NV_GPIN_RST, true);
  nxsig_usleep(120 * 1000);
}

static void nv_backlight(uint8_t level)
{
  FAR struct pwm_lowerhalf_s *pwm = rk3506_pwm_getlower();
  struct pwm_info_s info;

  if (pwm == NULL || pwm->ops == NULL)
    {
      return;
    }

  if (level == 0)
    {
      pwm->ops->stop(pwm);
      return;
    }

  memset(&info, 0, sizeof(info));
  info.frequency = NV_BL_FREQUENCY;
  info.duty = (ub16_t)(((uint32_t)level * 65535u) / CONFIG_LCD_MAXPOWER);
  pwm->ops->start(pwm, &info);
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
  pinfo->buffer  = g_linebuf;
  pinfo->bpp     = NV3007_BPP;
  pinfo->dev     = dev;
  return OK;
}

static int nv_putrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                     fb_coord_t col, FAR const uint8_t *buffer,
                     size_t npixels)
{
  nv_stream_begin(col, col + npixels - 1, row, row);
  nv_send_pixels(buffer, npixels);
  nv_deselect();
  return OK;
}

static int nv_putarea(FAR struct lcd_dev_s *dev,
                      fb_coord_t row_start, fb_coord_t row_end,
                      fb_coord_t col_start, fb_coord_t col_end,
                      FAR const uint8_t *buffer, fb_coord_t stride)
{
  unsigned int cols = col_end - col_start + 1;
  fb_coord_t row;

  nv_stream_begin(col_start, col_end, row_start, row_end);

  for (row = row_start; row <= row_end; row++)
    {
      nv_send_pixels(buffer, cols);
      buffer += stride;
    }

  nv_deselect();
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
 * Private Data (ops) and Public Functions
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

/****************************************************************************
 * Public Functions: NuttX lcd_framebuffer board hooks
 ****************************************************************************/

int board_lcd_initialize(void)
{
  static bool initialized;
  unsigned int i;

  if (initialized)
    {
      return OK;
    }

  memset(&g_nv, 0, sizeof(g_nv));
  g_nv.dev   = g_nv_ops;
  g_nv.power = 0;

  /* Backlight PWM first (routes RM_IO12/J9-29 and enables its clocks). */

  rk3506_pwm_initialize();

  /* Clock ungating is owned by the chip layer, not the board:
   * rk3506_gpio_config() ungates the GPIO bank it touches (the PMU bank
   * backing the RM_IO pads included) and rk3506_spibus_initialize()
   * ungates the SPI controller.  The old blanket
   * rk3506_all_clocks_enable() debug call and the explicit
   * rk3506_gpio0_clock_init() / rk3506_spi0_clock_init() here have all
   * been removed; the board only sequences pinmux + controller bring-up.
   */

  nv_pinctrl();

  g_nv.spi = rk3506_spibus_initialize(NV3007_SPI_BUS);
  if (g_nv.spi == NULL)
    {
      _err("NV3007: SPI%d init failed\n", NV3007_SPI_BUS);
      return -ENODEV;
    }

  nv_hw_reset();

  for (i = 0; i < sizeof(g_init) / sizeof(g_init[0]); i++)
    {
      nv_command(g_init[i].cmd, g_init[i].p, g_init[i].n);
      if (g_init[i].delay > 0)
        {
          nxsig_usleep((useconds_t)g_init[i].delay * 1000);
        }
    }

  nv_setpower(&g_nv.dev, CONFIG_LCD_MAXPOWER);

  initialized = true;

  _info("NV3007: %dx%d RGB565 on SPI0 J9 (CLK17/MOSI16/CS14, RST31/DC30/BL29), /dev/fb0\n",
        NV3007_XRES, NV3007_YRES);
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
