/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_st7701s.c
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
#include <errno.h>
#include <debug.h>
#include <string.h>
#include <nuttx/spinlock.h>

#include "hardware/rk3506_memorymap.h"
#include "hd_rk3506_st7701s.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef putreg32
#  define putreg32(v, x) (*(FAR volatile uint32_t *)(x) = (v))
#endif

#ifndef getreg32
#  define getreg32(x) (*(FAR volatile uint32_t *)(x))
#endif

/* ST7701S SPI GPIO pins (FlexSPI pins, GPIO bank 2) */

#define ST7701S_CS_PIN          64      /* GPIO2 PA0 */
#define ST7701S_SCL_PIN         65      /* GPIO2 PA1 */
#define ST7701S_SDI_PIN         66      /* GPIO2 PA2 */
#define ST7701S_RST_PIN         47      /* GPIO1 PC3 (shared with display reset) */

/* GPIO register offsets (Rockchip GPIOv2) */

#define GPIO_SWPORTA_DR_L       0x00
#define GPIO_SWPORTA_DR_H       0x04
#define GPIO_SWPORTA_DDR_L      0x08
#define GPIO_SWPORTA_DDR_H      0x0c

#define GPIO_PINS_PER_BANK      32
#define GPIO_BANK(pin)          ((pin) / GPIO_PINS_PER_BANK)
#define GPIO_PIN_IN_BANK(pin)   ((pin) % GPIO_PINS_PER_BANK)

/* SPI bit-bang timing */

#define ST7701S_SPI_DELAY_US    1

/* Reset timing */

#define ST7701S_RESET_HOLD_MS   20
#define ST7701S_RESET_WAIT_MS   120

/****************************************************************************
 * Private Data
 ****************************************************************************/

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

static void st7701s_gpio_set_direction(uint32_t pin, bool output)
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

static void st7701s_gpio_write(uint32_t pin, bool value)
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
 * Private Functions - SPI Bit-Bang
 ****************************************************************************/

/****************************************************************************
 * Name: st7701s_spi_write_9bit
 *
 * Description:
 *   Write a 9-bit value via 3-wire SPI bit-banging.
 *   Bit 8: DC flag (0=command, 1=data)
 *   Bits 7-0: data byte (MSB first)
 *
 ****************************************************************************/

static void st7701s_spi_write_9bit(uint16_t val)
{
  int i;

  /* Assert CS (active low) */

  st7701s_gpio_write(ST7701S_CS_PIN, false);
  up_udelay(ST7701S_SPI_DELAY_US);

  /* Clock out 9 bits, MSB first */

  for (i = 8; i >= 0; i--)
    {
      /* Set data bit */

      st7701s_gpio_write(ST7701S_SDI_PIN, (val >> i) & 1);
      up_udelay(ST7701S_SPI_DELAY_US);

      /* Rising edge: data sampled */

      st7701s_gpio_write(ST7701S_SCL_PIN, true);
      up_udelay(ST7701S_SPI_DELAY_US);

      /* Falling edge */

      st7701s_gpio_write(ST7701S_SCL_PIN, false);
      up_udelay(ST7701S_SPI_DELAY_US);
    }

  /* Deassert CS */

  st7701s_gpio_write(ST7701S_CS_PIN, true);
  up_udelay(ST7701S_SPI_DELAY_US);
}

/****************************************************************************
 * Name: st7701s_cmd
 *
 * Description:
 *   Send a command byte to ST7701S.
 *
 ****************************************************************************/

static void st7701s_cmd(uint8_t cmd)
{
  st7701s_spi_write_9bit((uint16_t)cmd);  /* DC=0 for command */
}

/****************************************************************************
 * Name: st7701s_data
 *
 * Description:
 *   Send a data byte to ST7701S.
 *
 ****************************************************************************/

static void st7701s_data(uint8_t data)
{
  st7701s_spi_write_9bit(0x100 | (uint16_t)data);  /* DC=1 for data */
}

/****************************************************************************
 * Name: st7701s_reset
 *
 * Description:
 *   Perform hardware reset of the ST7701S panel.
 *
 ****************************************************************************/

static void st7701s_reset(void)
{
  /* Configure RST pin as output */

  st7701s_gpio_set_direction(ST7701S_RST_PIN, true);

  /* Assert reset (active low) */

  st7701s_gpio_write(ST7701S_RST_PIN, false);
  up_mdelay(ST7701S_RESET_HOLD_MS);

  /* Deassert reset */

  st7701s_gpio_write(ST7701S_RST_PIN, true);
  up_mdelay(ST7701S_RESET_WAIT_MS);
}

/****************************************************************************
 * Name: st7701s_init_sequence
 *
 * Description:
 *   Send the ST7701S initialization sequence for 480x854 RGB565.
 *   Based on ST7701S datasheet and ArtInChip reference driver.
 *
 ****************************************************************************/

static void st7701s_init_sequence(void)
{
  /* Command2 BK3 select */

  st7701s_cmd(0xff);
  st7701s_data(0x77);
  st7701s_data(0x01);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x13);

  st7701s_cmd(0xef);
  st7701s_data(0x08);

  /* Command2 BK0 select */

  st7701s_cmd(0xff);
  st7701s_data(0x77);
  st7701s_data(0x01);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x10);

  /* Display line setting */

  st7701s_cmd(0xc0);
  st7701s_data(0x77);
  st7701s_data(0x00);

  /* Porch control */

  st7701s_cmd(0xc1);
  st7701s_data(0x0e);
  st7701s_data(0x0c);

  /* Inversion selection */

  st7701s_cmd(0xc2);
  st7701s_data(0x07);
  st7701s_data(0x02);

  st7701s_cmd(0xc3);
  st7701s_data(0x00);

  st7701s_cmd(0xcc);
  st7701s_data(0x30);

  st7701s_cmd(0xcd);
  st7701s_data(0x08);

  /* Positive voltage gamma control */

  st7701s_cmd(0xb0);
  st7701s_data(0x00);
  st7701s_data(0x17);
  st7701s_data(0x1f);
  st7701s_data(0x0e);
  st7701s_data(0x11);
  st7701s_data(0x06);
  st7701s_data(0x0d);
  st7701s_data(0x08);
  st7701s_data(0x07);
  st7701s_data(0x26);
  st7701s_data(0x03);
  st7701s_data(0x11);
  st7701s_data(0x0f);
  st7701s_data(0x2a);
  st7701s_data(0x31);
  st7701s_data(0x1c);

  /* Negative voltage gamma control */

  st7701s_cmd(0xb1);
  st7701s_data(0x00);
  st7701s_data(0x17);
  st7701s_data(0x1f);
  st7701s_data(0x0d);
  st7701s_data(0x11);
  st7701s_data(0x07);
  st7701s_data(0x0c);
  st7701s_data(0x08);
  st7701s_data(0x08);
  st7701s_data(0x26);
  st7701s_data(0x04);
  st7701s_data(0x11);
  st7701s_data(0x0f);
  st7701s_data(0x2a);
  st7701s_data(0x31);
  st7701s_data(0x1c);

  /* Command2 BK1 select */

  st7701s_cmd(0xff);
  st7701s_data(0x77);
  st7701s_data(0x01);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x11);

  /* Vop amplitude setting */

  st7701s_cmd(0xb0);
  st7701s_data(0x5c);

  /* VCOM amplitude setting */

  st7701s_cmd(0xb1);
  st7701s_data(0x60);

  /* VGH voltage setting */

  st7701s_cmd(0xb2);
  st7701s_data(0x07);

  /* TEST command setting */

  st7701s_cmd(0xb3);
  st7701s_data(0x80);

  /* VGL voltage setting */

  st7701s_cmd(0xb5);
  st7701s_data(0x49);

  /* Power control 1 */

  st7701s_cmd(0xb7);
  st7701s_data(0x87);

  /* Power control 2 */

  st7701s_cmd(0xb8);
  st7701s_data(0x22);

  /* Source pre_drive timing set1 */

  st7701s_cmd(0xc1);
  st7701s_data(0x78);

  /* Source EQ2 setting */

  st7701s_cmd(0xc2);
  st7701s_data(0x78);

  /* MIPI setting 1 */

  st7701s_cmd(0xd0);
  st7701s_data(0x88);

  up_mdelay(100);

  /* Power saving */

  st7701s_cmd(0xe0);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x02);

  st7701s_cmd(0xe1);
  st7701s_data(0x03);
  st7701s_data(0x96);
  st7701s_data(0x05);
  st7701s_data(0x96);
  st7701s_data(0x02);
  st7701s_data(0x96);
  st7701s_data(0x04);
  st7701s_data(0x96);
  st7701s_data(0x00);
  st7701s_data(0x44);
  st7701s_data(0x44);

  st7701s_cmd(0xe2);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x03);
  st7701s_data(0x03);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x02);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x02);
  st7701s_data(0x00);

  st7701s_cmd(0xe3);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x33);
  st7701s_data(0x33);

  st7701s_cmd(0xe4);
  st7701s_data(0x44);
  st7701s_data(0x44);

  st7701s_cmd(0xe5);
  st7701s_data(0x0b);
  st7701s_data(0xd4);
  st7701s_data(0x28);
  st7701s_data(0x8c);
  st7701s_data(0x0d);
  st7701s_data(0xd6);
  st7701s_data(0x28);
  st7701s_data(0x8c);
  st7701s_data(0x07);
  st7701s_data(0xd0);
  st7701s_data(0x28);
  st7701s_data(0x8c);
  st7701s_data(0x09);
  st7701s_data(0xd2);
  st7701s_data(0x28);
  st7701s_data(0x8c);

  st7701s_cmd(0xe6);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x33);
  st7701s_data(0x33);

  st7701s_cmd(0xe7);
  st7701s_data(0x44);
  st7701s_data(0x44);

  st7701s_cmd(0xe8);
  st7701s_data(0x0a);
  st7701s_data(0xd5);
  st7701s_data(0x28);
  st7701s_data(0x8c);
  st7701s_data(0x0c);
  st7701s_data(0xd7);
  st7701s_data(0x28);
  st7701s_data(0x8c);
  st7701s_data(0x06);
  st7701s_data(0xd1);
  st7701s_data(0x28);
  st7701s_data(0x8c);
  st7701s_data(0x08);
  st7701s_data(0xd3);
  st7701s_data(0x28);
  st7701s_data(0x8c);

  st7701s_cmd(0xeb);
  st7701s_data(0x00);
  st7701s_data(0x01);
  st7701s_data(0xe4);
  st7701s_data(0xe4);
  st7701s_data(0x44);
  st7701s_data(0x00);

  st7701s_cmd(0xed);
  st7701s_data(0xff);
  st7701s_data(0x45);
  st7701s_data(0x67);
  st7701s_data(0xfc);
  st7701s_data(0x01);
  st7701s_data(0x3f);
  st7701s_data(0xab);
  st7701s_data(0xff);
  st7701s_data(0xff);
  st7701s_data(0xba);
  st7701s_data(0xf3);
  st7701s_data(0x10);
  st7701s_data(0xcf);
  st7701s_data(0x76);
  st7701s_data(0x54);
  st7701s_data(0xff);

  st7701s_cmd(0xef);
  st7701s_data(0x08);
  st7701s_data(0x08);
  st7701s_data(0x08);
  st7701s_data(0x45);
  st7701s_data(0x3f);
  st7701s_data(0x54);

  /* Command2 disable */

  st7701s_cmd(0xff);
  st7701s_data(0x77);
  st7701s_data(0x01);
  st7701s_data(0x00);
  st7701s_data(0x00);
  st7701s_data(0x00);

  /* Tear effect line ON */

  st7701s_cmd(0x35);
  st7701s_data(0x00);

  /* Interface pixel format: 18bit/pixel */

  st7701s_cmd(0x3a);
  st7701s_data(0x66);

  /* Sleep out */

  st7701s_cmd(0x11);
  up_mdelay(120);

  /* Display on */

  st7701s_cmd(0x29);
  up_mdelay(20);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_st7701s_initialize
 ****************************************************************************/

int hd_rk3506_st7701s_initialize(void)
{
  /* Configure SPI GPIO pins as outputs */

  st7701s_gpio_set_direction(ST7701S_CS_PIN, true);
  st7701s_gpio_set_direction(ST7701S_SCL_PIN, true);
  st7701s_gpio_set_direction(ST7701S_SDI_PIN, true);

  /* Set initial pin states */

  st7701s_gpio_write(ST7701S_CS_PIN, true);   /* CS deasserted */
  st7701s_gpio_write(ST7701S_SCL_PIN, false);  /* SCL low */
  st7701s_gpio_write(ST7701S_SDI_PIN, false);  /* SDI low */

  /* Hardware reset */

  st7701s_reset();

  /* Send initialization sequence */

  st7701s_init_sequence();

  syslog(LOG_INFO, "ST7701S LCD panel initialized: 480x854 RGB565\n");
  return OK;
}
