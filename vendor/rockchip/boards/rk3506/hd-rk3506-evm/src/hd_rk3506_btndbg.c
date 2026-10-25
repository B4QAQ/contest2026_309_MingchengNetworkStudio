/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_btndbg.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NSH "btn" diagnostic for the two J9 buttons (NEXT = J9-33/GPIO1_D1,
 * OK = J9-32/GPIO1_D2, both plain non-RM_IO pads).
 *
 * Prints, per pin:
 *   - raw pad level via the GPIO driver (1=released, 0=PRESSED)
 *   - the IOMUX word and the decoded 4-bit mux field
 *   - the pull word and the decoded 2-bit pull field
 *   - the GPIO block EXT_PORT raw bit
 *
 * Register maps per the Linux SDK pinctrl-rk3506.c:
 *   bank0: IOC 0xff950000, mux A@0x00 B@0x08 C@0x10, pull base 0x200
 *   bank1: IOC 0xff660000, mux A@0x20 B@0x28 C@0x30 D@0x38, pull 0x210
 *   GPIO0 EXT_PORT = 0xff940000 + 0x70, GPIO1 = 0xff870000 + 0x70
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdio.h>

#include "drv/gpio/rk3506_gpio.h"

#if defined(CONFIG_HD_RK3506_LV_BUTTONS)

static uint32_t btn_getreg(uintptr_t addr)
{
  return *(volatile uint32_t *)addr;
}

static void btn_dump_pin(const char *name, uint8_t gpin)
{
  static const uint32_t ioc_base[2]  = { 0xff950000u, 0xff660000u };
  static const uint32_t mux_base[2]  = { 0x00u, 0x20u };
  static const uint32_t pull_base[2] = { 0x200u, 0x210u };
  static const uint32_t gpio_base[2] = { 0xff940000u, 0xff870000u };

  uint8_t  bank = RK3506_GPIO_BANK(gpin);
  uint8_t  pin  = RK3506_GPIO_PINNO(gpin);
  uint32_t iomux_reg;
  uint32_t iomux_bit;
  uint32_t pull_reg;
  uint32_t pull_bit;
  uint32_t ioc;
  uint32_t pull;
  uint32_t ext;
  bool     level = false;
  int      ret;

  if (bank > 1u)
    {
      printf("%s: bank%u not covered by this diagnostic\n", name, bank);
      return;
    }

  iomux_reg = mux_base[bank] + (uint32_t)(pin / 8u) * 0x08u +
              (((pin % 8u) >= 4u) ? 4u : 0u);
  iomux_bit = (uint32_t)(pin % 4u) * 4u;
  pull_reg  = pull_base[bank] + (uint32_t)(pin / 8u) * 4u;
  pull_bit  = (uint32_t)(pin % 8u) * 2u;

  ioc  = btn_getreg(ioc_base[bank] + iomux_reg);
  pull = btn_getreg(ioc_base[bank] + pull_reg);
  ext  = btn_getreg(gpio_base[bank] + 0x70u);

  ret = rk3506_gpio_read(gpin, &level);

  printf("%s (bank%u pin%u):\n", name, bank, pin);
  printf("  IOMUX [0x%03x] = 0x%08x  -> mux field = %u\n",
         (unsigned)iomux_reg, (unsigned)ioc,
         (unsigned)((ioc >> iomux_bit) & 0xfu));
  printf("  PULL  [0x%03x] = 0x%08x  -> pull field = %u\n",
         (unsigned)pull_reg, (unsigned)pull,
         (unsigned)((pull >> pull_bit) & 0x3u));
  printf("  EXT_PORT bit%u = %u   gpio_read = %s\n",
         pin, (unsigned)((ext >> pin) & 1u),
         ret < 0 ? "ERROR" : (level ? "1 released" : "0 PRESSED"));
}

int btn_main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  btn_dump_pin("NEXT J9-33 GPIO1_D1", RK3506_GPIO_PIN(1, 25));
  btn_dump_pin("OK   J9-32 GPIO1_D2", RK3506_GPIO_PIN(1, 26));
  btn_dump_pin("OLD  J9-18 GPIO0_C2 (RM_IO ref)", RK3506_GPIO_PIN(0, 18));
  printf("(mux 0=GPIO, pull 1=up; hold a button and re-run to watch "
         "EXT_PORT flip)\n");
  return 0;
}

#endif /* CONFIG_HD_RK3506_LV_BUTTONS */
