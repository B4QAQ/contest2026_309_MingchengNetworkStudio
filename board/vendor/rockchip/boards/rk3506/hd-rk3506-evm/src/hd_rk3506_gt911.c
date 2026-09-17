/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_gt911.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <syslog.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <string.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_i2c.h"
#include "hd_rk3506_gt911.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef putreg32
#  define putreg32(v, x) (*(FAR volatile uint32_t *)(x) = (v))
#endif

#ifndef getreg32
#  define getreg32(x) (*(FAR volatile uint32_t *)(x))
#endif

/* GT911 maximum report frame size */

#define GT911_BUFFER_SIZE     41
#define GT911_MAX_TOUCH       5

/* GT911 board configuration */

#define GT911_I2C_BUS         0       /* I2C0 */
#define GT911_I2C_ADDR        0x5d    /* 7-bit address */
#define GT911_I2C_CLOCK       400000  /* 400kHz */

/* GPIO pins for GT911 (absolute pin numbers: bank * 32 + in-bank pin,
 * in-bank: A0..A7=0..7, B0..B7=8..15, C0..C7=16..23, D0..D7=24..31).
 * Per the SDK dts (rk3506-iot-nand-rgb-800x480.dtsi):
 *   irq-gpios = <&gpio1 RK_PC2 0>  -> GPIO1_C2 = 32 + 18 = 50
 *   reset-gpios = <&gpio4 RK_PA4 GPIO_ACTIVE_HIGH> -> GPIO4_A4 = 128 + 4 = 132
 * NOTE (BUG HISTORY M4.1): the previous values (60/128) actually
 * addressed GPIO1_D4 / GPIO4_A0, so the reset/INT sequence toggled
 * the wrong pins entirely.
 */

#define GT911_IRQ_PIN         50      /* GPIO1_C2 */
#define GT911_RST_PIN         132     /* GPIO4_A4 */

/* Display resolution limits (ST7701S 480x854 portrait) */

#define GT911_MAX_X           480
#define GT911_MAX_Y           854

/* Touch rotation: GT911 reports landscape coordinates (854x480),
 * display is portrait (480x854). Enable rotation to map correctly.
 */

#define GT911_ROTATE_90       1

#define GT911_PATH            "/dev/input0"
#define GT911_WORK_DELAY      10      /* 10ms polling interval */
#define GT911_SAMPLE_CACHES   16

/* GT911 registers address */

#define GT911_READ_XY_REG     0x814e
#define GT911_READ_DATA_REG   0x814f
#define GT911_CONFIG_REG      0x8047
#define GT911_PRODUCT_ID_REG  0x8140

/* RK3506 GPIO register offsets (standard Rockchip GPIOv2 controller) */

#define GPIO_SWPORTA_DR_L     0x00    /* Data register, pins 0-15 */
#define GPIO_SWPORTA_DR_H     0x04    /* Data register, pins 16-31 */
#define GPIO_SWPORTA_DDR_L    0x08    /* Direction register, pins 0-15 */
#define GPIO_SWPORTA_DDR_H    0x0c    /* Direction register, pins 16-31 */
#define GPIO_EXT_PORTA        0x70    /* External port register (read) */

/* GPIO helper macros */

#define GPIO_PINS_PER_BANK    32
#define GPIO_BANK(pin)        ((pin) / GPIO_PINS_PER_BANK)
#define GPIO_PIN_IN_BANK(pin) ((pin) % GPIO_PINS_PER_BANK)

/* GT911 reset timing (milliseconds) */

#define GT911_RESET_HOLD_MS   20
#define GT911_RESET_WAIT_MS   50

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes the state of one GT911 driver instance */

struct gt911_dev_s
{
  struct touch_lowerhalf_s  touch_lower;    /* Touchscreen lowerhalf */

  bool                has_report;           /* Mark if report event */

  struct i2c_master_s *i2c;                 /* I2C master port */
  struct work_s       work;                 /* Read sample data work */
  spinlock_t          lock;                 /* Device specific lock */

  uint8_t buffer[GT911_BUFFER_SIZE];        /* Read buffer */
};

/* This structure describes the frame of touchpoint */

begin_packed_struct struct gt911_touchpoint_s
{
  uint8_t id;                               /* Touch point ID */
  uint16_t x;                               /* Touch X-axis */
  uint16_t y;                               /* Touch Y-axis */
  uint16_t pressure;                        /* Touch pressure */
  uint8_t  reserved;                        /* Reserved */
} end_packed_struct;

/* This structure describes the report data header */

begin_packed_struct struct gt911_data_s
{
  uint8_t touchpoints     : 4;              /* Touch point number */
  uint8_t has_key         : 1;              /* 1: key is pressed */
  uint8_t proximity_valid : 1;              /* Proximity valid */
  uint8_t large_detected  : 1;              /* 1: large-area touch */
  uint8_t buffer_status   : 1;              /* 1: input data is valid */

  struct gt911_touchpoint_s touchpoint[0];  /* Touch point data */
} end_packed_struct;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct gt911_dev_s g_gt911_dev;

/* GPIO bank base address lookup table */

static const uintptr_t g_gpio_bank_base[5] =
{
  RK3506_GPIO0_ADDR,
  RK3506_GPIO1_ADDR,
  RK3506_GPIO2_ADDR,
  RK3506_GPIO3_ADDR,
  RK3506_GPIO4_ADDR
};

/****************************************************************************
 * Private Functions - GPIO
 ****************************************************************************/

/****************************************************************************
 * Name: gt911_gpio_set_direction
 *
 * Description:
 *   Configure a GPIO pin as input or output.
 *
 * Input Parameters:
 *   pin    - Absolute GPIO pin number
 *   output - true for output, false for input
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt911_gpio_set_direction(uint32_t pin, bool output)
{
  uintptr_t base;
  uint32_t bank_pin;
  uintptr_t reg_offset;
  uint32_t bit;
  uint32_t val;

  base = g_gpio_bank_base[GPIO_BANK(pin)];
  bank_pin = GPIO_PIN_IN_BANK(pin);

  if (bank_pin < 16)
    {
      reg_offset = GPIO_SWPORTA_DDR_L;
      bit = bank_pin;
    }
  else
    {
      reg_offset = GPIO_SWPORTA_DDR_H;
      bit = bank_pin - 16;
    }

  val = getreg32(base + reg_offset);
  if (output)
    {
      val |= (1u << bit);
    }
  else
    {
      val &= ~(1u << bit);
    }

  putreg32(val, base + reg_offset);
}

/****************************************************************************
 * Name: gt911_gpio_write
 *
 * Description:
 *   Write a value to an output GPIO pin.
 *
 * Input Parameters:
 *   pin   - Absolute GPIO pin number
 *   value - true for high, false for low
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt911_gpio_write(uint32_t pin, bool value)
{
  uintptr_t base;
  uint32_t bank_pin;
  uintptr_t reg_offset;
  uint32_t bit;
  uint32_t val;

  base = g_gpio_bank_base[GPIO_BANK(pin)];
  bank_pin = GPIO_PIN_IN_BANK(pin);

  if (bank_pin < 16)
    {
      reg_offset = GPIO_SWPORTA_DR_L;
      bit = bank_pin;
    }
  else
    {
      reg_offset = GPIO_SWPORTA_DR_H;
      bit = bank_pin - 16;
    }

  val = getreg32(base + reg_offset);
  if (value)
    {
      val |= (1u << bit);
    }
  else
    {
      val &= ~(1u << bit);
    }

  putreg32(val, base + reg_offset);
}

/****************************************************************************
 * Private Functions - I2C
 ****************************************************************************/

/****************************************************************************
 * Name: gt911_read_reg
 *
 * Description:
 *   Read GT911 continuous registers value.
 *
 * Input Parameters:
 *   dev    - GT911 object pointer
 *   reg    - Register start address
 *   buf    - Register value buffer
 *   buflen - Register value buffer length
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int gt911_read_reg(struct gt911_dev_s *dev,
                          uint16_t reg,
                          uint8_t *buf,
                          int buflen)
{
  int ret;

  /* Send the Register Address, MSB first */

  uint8_t regbuf[2] =
  {
    reg >> 8,   /* First Byte: MSB */
    reg & 0xff  /* Second Byte: LSB */
  };

  /* Compose the I2C Messages */

  struct i2c_msg_s msgv[2] =
  {
    {
      /* Send the I2C Register Address */

      .frequency = GT911_I2C_CLOCK,
      .addr      = GT911_I2C_ADDR,
      .flags     = 0,
      .buffer    = regbuf,
      .length    = sizeof(regbuf)
    },
    {
      /* Receive the I2C Register Values */

      .frequency = GT911_I2C_CLOCK,
      .addr      = GT911_I2C_ADDR,
      .flags     = I2C_M_READ,
      .buffer    = buf,
      .length    = buflen
    }
  };

  const int msgv_len = sizeof(msgv) / sizeof(msgv[0]);

  _info("reg=0x%x, buflen=%d\n", reg, buflen);
  DEBUGASSERT(dev && dev->i2c && buf);

  /* Execute the I2C Transfer */

  ret = I2C_TRANSFER(dev->i2c, msgv, msgv_len);
  if (ret < 0)
    {
      _err("I2C Read failed: %d\n", ret);
      return ret;
    }

#ifdef CONFIG_DEBUG_INPUT_INFO
  iinfodumpbuffer("gt911_read_reg", buf, buflen);
#endif

  return 0;
}

/****************************************************************************
 * Name: gt911_write_reg
 *
 * Description:
 *   Write GT911 register value.
 *
 * Input Parameters:
 *   dev - GT911 object pointer
 *   reg - Register address
 *   val - Register value
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int gt911_write_reg(struct gt911_dev_s *dev,
                           uint16_t reg,
                           uint8_t val)
{
  int ret;

  /* Send the Register Address, MSB first */

  uint8_t regbuf[3] =
  {
    reg >> 8,   /* First Byte: MSB */
    reg & 0xff, /* Second Byte: LSB */
    val,
  };

  /* Compose the I2C Messages */

  struct i2c_msg_s msgv[1] =
  {
    {
      /* Send the I2C Register Address + Data */

      .frequency = GT911_I2C_CLOCK,
      .addr      = GT911_I2C_ADDR,
      .flags     = 0,
      .buffer    = regbuf,
      .length    = sizeof(regbuf)
    }
  };

  const int msgv_len = sizeof(msgv) / sizeof(msgv[0]);

  _info("reg=0x%x, val=%d\n", reg, val);
  DEBUGASSERT(dev && dev->i2c);

  /* Execute the I2C Transfer */

  ret = I2C_TRANSFER(dev->i2c, msgv, msgv_len);
  if (ret < 0)
    {
      _err("I2C Write failed: %d\n", ret);
      return ret;
    }

  return 0;
}

/****************************************************************************
 * Private Functions - GT911 Control
 ****************************************************************************/

/****************************************************************************
 * Name: gt911_reset
 *
 * Description:
 *   Perform hardware reset of the GT911 controller via GPIO pins,
 *   following the SDK vendor driver (gt9xx.c gtp_reset_guitar()
 *   + gtp_int_sync()) exactly:
 *
 *     1. RST low, hold >10ms                          (T2)
 *     2. INT low  -> I2C addr 0xBA/0xBB (7-bit 0x5D)  (SDK: "HIGH:
 *        0x28/0x29, LOW: 0xBA/0xBB", our client addr is 0x5D)
 *     3. wait >100us                                  (T3)
 *     4. RST high, wait >5ms                          (T4)
 *     5. RST back to high-impedance input (end addr select)
 *     6. INT low 50ms, then INT to input (int sync)
 *
 *   BUG HISTORY (M4.1): the previous sequence never returned RST to
 *   input and had no T3 wait; combined with the wrong pin numbers
 *   the controller was never reset correctly.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt911_reset(void)
{
  /* Configure RST and IRQ pins as GPIO outputs */

  gt911_gpio_set_direction(GT911_RST_PIN, true);
  gt911_gpio_set_direction(GT911_IRQ_PIN, true);

  /* T2: RST low */

  gt911_gpio_write(GT911_RST_PIN, false);
  up_mdelay(GT911_RESET_HOLD_MS);        /* 20ms > 10ms */

  /* Address select: INT LOW -> 0x5D (0xBA/0xBB) */

  gt911_gpio_write(GT911_IRQ_PIN, false);
  up_mdelay(2);                          /* T3 > 100us */

  /* T4: RST high */

  gt911_gpio_write(GT911_RST_PIN, true);
  up_mdelay(6);                          /* T4 > 5ms */

  /* End of address select: RST back to high-impedance input
   * (the board has an external pull-up on the RST net).
   */

  gt911_gpio_set_direction(GT911_RST_PIN, false);

  /* gtp_int_sync(): INT low 50ms, then INT to input for interrupts */

  gt911_gpio_write(GT911_IRQ_PIN, false);
  up_mdelay(GT911_RESET_WAIT_MS);        /* 50ms */
  gt911_gpio_set_direction(GT911_IRQ_PIN, false);
}

/****************************************************************************
 * Name: gt911_verify_product_id
 *
 * Description:
 *   Read the product ID registers and verify the GT911 is present.
 *
 * Input Parameters:
 *   dev - GT911 object pointer
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int gt911_verify_product_id(struct gt911_dev_s *dev)
{
  uint8_t product_id[5];
  int ret;

  memset(product_id, 0, sizeof(product_id));
  ret = gt911_read_reg(dev, GT911_PRODUCT_ID_REG, product_id, 4);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to read GT911 product ID: %d\n", ret);
      return ret;
    }

  /* GT911 returns ASCII "911\0" at register 0x8140 */

  if (memcmp(product_id, "911", 3) != 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 not found, got: %02x %02x %02x %02x\n",
             product_id[0], product_id[1],
             product_id[2], product_id[3]);
      return -ENODEV;
    }

  syslog(LOG_INFO, "GT911 detected, product ID: %.4s\n", product_id);
  return OK;
}

/****************************************************************************
 * Private Functions - Touch Event Processing
 ****************************************************************************/

/****************************************************************************
 * Name: gt911_touch_event
 *
 * Description:
 *   Process touch event. Read touchpoint data and send to touch event.
 *   Coordinates are rotated 90° and clamped to the display resolution
 *   (480x854 portrait).
 *
 * Input Parameters:
 *   dev - GT911 object pointer
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt911_touch_event(struct gt911_dev_s *dev)
{
  struct gt911_data_s *data;
  struct gt911_touchpoint_s *tp;
  struct touch_sample_s sample;
  struct touch_point_s *point = sample.point;
  irqstate_t flags;
  uint16_t raw_x;
  uint16_t raw_y;

  memset(&sample, 0, sizeof(sample));
  sample.npoints = 1;

  flags = spin_lock_irqsave(&dev->lock);

  data = (struct gt911_data_s *)dev->buffer;
  tp = data->touchpoint;

  raw_x = tp->x;
  raw_y = tp->y;

#if GT911_ROTATE_90
  /* Rotate 90°: landscape (854x480) → portrait (480x854)
   * display_x = raw_y
   * display_y = (854 - 1) - raw_x
   */

  point->x = raw_y;
  point->y = (GT911_MAX_Y - 1) - raw_x;
#else
  point->x = raw_x;
  point->y = raw_y;
#endif

  /* Clamp coordinates to display resolution */

  if (point->x >= GT911_MAX_X)
    {
      point->x = GT911_MAX_X - 1;
    }

  if (point->y >= GT911_MAX_Y)
    {
      point->y = GT911_MAX_Y - 1;
    }

  point->pressure  = tp->pressure;
  point->flags     = TOUCH_POS_VALID | TOUCH_PRESSURE_VALID;

  if (data->buffer_status)
    {
      point->flags |= TOUCH_DOWN;
      dev->has_report = true;
    }
  else
    {
      point->flags |= TOUCH_UP;
      dev->has_report = false;
    }

  spin_unlock_irqrestore(&dev->lock, flags);

  touch_event(dev->touch_lower.priv, &sample);
}

/****************************************************************************
 * Name: gt911_event
 *
 * Description:
 *   Process GT911 event.
 *
 * Input Parameters:
 *   dev - GT911 object pointer
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt911_event(struct gt911_dev_s *dev)
{
  struct gt911_data_s *data = (struct gt911_data_s *)dev->buffer;

  if (!data->has_key)
    {
      gt911_touch_event(dev);
    }
  else
    {
      _err("ERROR: event is invalid\n");
    }
}

/****************************************************************************
 * Name: gt911_worker
 *
 * Description:
 *   Process GT911 work, read GT911 report frame and process it.
 *
 * Input Parameters:
 *   arg - GT911 object pointer
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt911_worker(void *arg)
{
  int ret;
  struct gt911_dev_s *dev = (struct gt911_dev_s *)arg;
  struct gt911_data_s *data;
  clock_t delay = GT911_WORK_DELAY;
  bool touched = false;
  irqstate_t flags;

  /* Read the status byte at 0x814E */

  ret = gt911_read_reg(dev, GT911_READ_XY_REG, dev->buffer, 1);
  if (ret != 0)
    {
      _err("ERROR: I2C read status failed: %d\n", ret);
      goto exit;
    }

  flags = spin_lock_irqsave(&dev->lock);

  data = (struct gt911_data_s *)dev->buffer;

  /* Check if buffer is ready and touch points are valid */

  if (data->buffer_status &&
      (data->touchpoints > 0) &&
      (data->touchpoints < GT911_MAX_TOUCH))
    {
      /* Read the touch point data */

      ret = gt911_read_reg(dev, GT911_READ_DATA_REG,
                           &dev->buffer[1], data->touchpoints * 8);
      if (ret != 0)
        {
          spin_unlock_irqrestore(&dev->lock, flags);
          _err("ERROR: I2C read data failed: %d\n", ret);
          goto exit;
        }

      touched = true;
    }
  else if (dev->has_report)
    {
      /* Previously reported touch, now release */

      touched = true;
    }

  spin_unlock_irqrestore(&dev->lock, flags);

  /* Clear the status register by writing 0 to 0x814E */

  ret = gt911_write_reg(dev, GT911_READ_XY_REG, 0);
  if (ret != 0)
    {
      _err("ERROR: I2C write clear failed: %d\n", ret);
      goto exit;
    }

  if (touched)
    {
      gt911_event(dev);
      delay = 1;
    }

exit:
  ret = work_queue(LPWORK, &dev->work, gt911_worker, dev, delay);
  if (ret != 0)
    {
      _err("ERROR: work_queue() failed: %d\n", ret);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_gt911_initialize
 *
 * Description:
 *   Initialize GT911 touchscreen on I2C0.
 *
 *   This function performs the following steps:
 *   1. Initialize the I2C0 bus
 *   2. Reset the GT911 via GPIO (RST/IRQ pins)
 *   3. Verify GT911 presence by reading product ID
 *   4. Register the touchscreen device at /dev/input0
 *   5. Start periodic polling via work queue (10ms interval)
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int hd_rk3506_gt911_initialize(void)
{
  int ret;
  struct gt911_dev_s *dev = &g_gt911_dev;

  memset(dev, 0, sizeof(*dev));

  /* Perform hardware reset via GPIO */

  gt911_reset();

  /* Initialize I2C0 bus */

  dev->i2c = rk3506_i2cbus_initialize(GT911_I2C_BUS);
  if (!dev->i2c)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize I2C bus %d\n",
             GT911_I2C_BUS);
      return -ENODEV;
    }

  /* Verify GT911 is present by reading product ID */

  ret = gt911_verify_product_id(dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 not detected: %d\n", ret);
      goto errout_i2c;
    }

  /* Register the touchscreen device */

  ret = touch_register(&dev->touch_lower, GT911_PATH,
                       GT911_SAMPLE_CACHES);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: touch_register() failed: %d\n", ret);
      goto errout_i2c;
    }

  /* Start the work queue for polling touch events */

  ret = work_queue(LPWORK, &dev->work, gt911_worker,
                   dev, GT911_WORK_DELAY);
  if (ret != 0)
    {
      syslog(LOG_ERR, "ERROR: work_queue() failed: %d\n", ret);
      goto errout_touch;
    }

  syslog(LOG_INFO, "GT911 touchscreen initialized on I2C0, "
         "addr=0x%02x, max=%dx%d, rotate=%d\n",
         GT911_I2C_ADDR, GT911_MAX_X, GT911_MAX_Y,
         GT911_ROTATE_90 ? 90 : 0);
  return OK;

errout_touch:
  touch_unregister(&dev->touch_lower, GT911_PATH);

errout_i2c:
  dev->i2c = NULL;
  return ret;
}
