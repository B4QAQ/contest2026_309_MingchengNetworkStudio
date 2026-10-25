/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_btndbg.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NSH "btn" diagnostic for the two J9 buttons (NEXT = J9-18/GPIO0_C2/
 * pin18, OK = J9-20/GPIO0_C4/pin20).
 *
 * Prints, per pin:
 *   - raw pad level via the GPIO driver (1=released, 0=PRESSED)
 *   - the IOC_PMU IOMUX word and the decoded 4-bit mux field
 *   - the IOC_PMU pull word and the decoded 2-bit pull field
 *   - GPIO0 EXT_PORT raw bit
 *
 * Register maps per the Linux SDK pinctrl-rk3506.c:
 *   IOMUX GPIO0C_CON = IOC_PMU(0xff950000) + 0x10 (C0..C3) / 0x14 (C4..C7)
 *   PULL  bank0      = IOC_PMU + 0x200 + (pin/8)*4, 2 bits per pin
 *   GPIO0 EXT_PORT   = GPIO0(0xff940000) + 0x70
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdio.h>

#include "drv/gpio/rk3506_gpio.h"
#include "hardware/rk3506_memorymap.h"

#if defined(CONFIG_HD_RK3506_LV_BUTTONS)

#define BTN_IOC_PMU    0xff950000u
#define BTN_GPIO0      0xff940000u

static uint32_t btn_getreg(uintptr_t addr)
{
  return *(volatile uint32_t *)addr;
}

static void btn_dump_pin(const char *name, uint8_t pin)
{
  uint32_t iomux_reg = (uint32_t)(pin / 8u) * 0x08u +
                       (((pin % 8u) >= 4u) ? 4u : 0u);
  uint32_t iomux_bit = (uint32_t)(pin % 4u) * 4u;
  uint32_t pull_reg  = 0x200u + (uint32_t)(pin / 8u) * 4u;
  uint32_t pull_bit  = (uint32_t)(pin % 8u) * 2u;
  uint32_t ioc;
  uint32_t pull;
  uint32_t ext;
  bool     level = false;
  int      ret;

  ioc  = btn_getreg(BTN_IOC_PMU + iomux_reg);
  pull = btn_getreg(BTN_IOC_PMU + pull_reg);
  ext  = btn_getreg(BTN_GPIO0 + 0x70u);

  ret = rk3506_gpio_read(RK3506_GPIO_PIN(0, pin), &level);

  printf("%s (pin%u):\n", name, pin);
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

  btn_dump_pin("NEXT J9-18 GPIO0_C2 (RM_IO)", 18);
  btn_dump_pin("OK   J9-20 GPIO0_C4 (RM_IO)", 20);
  btn_dump_pin("REF  J9-31 GPIO0_B0 (plain)", 8);
  printf("(mux 0=GPIO, pull 1=up; hold a button and re-run to watch "
         "EXT_PORT flip)\n");
  return 0;
}

#endif /* CONFIG_HD_RK3506_LV_BUTTONS */
