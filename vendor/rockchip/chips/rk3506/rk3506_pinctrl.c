/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_pinctrl.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unified pin-control (IOMUX / RM_IO / pull / drive / schmitt) for the
 * Rockchip RK3506G2.
 *
 * Register offsets, bit fields and write sequences are taken verbatim
 * from the vendor SDK:
 *
 *   U-Boot  drivers/pinctrl/rockchip/pinctrl-rk3506.c
 *           rk3506_set_mux()          :62-103
 *           rockchip_set_rmio()       :14-60
 *           rk3506_calc_drv_reg_and_bit()   :114-188
 *           rk3506_set_drive()        :190-217
 *           rk3506_calc_pull_reg_and_bit()  :228-302
 *           rk3506_set_pull()         :304-337
 *           rk3506_calc_schmitt_reg_and_bit():348-423
 *           rk3506_set_schmitt()      :425-448
 *           rk3506_pin_banks[]        :460-491
 *   CMSIS   hal/lib/CMSIS/Device/RK3506/Include/rk3506.h
 *           struct GPIO0_IOC_REG      :1951-1999
 *           struct RM_IO_REG          :1834-1874
 *
 * All writes use the Rockchip HIWORD_UPDATE convention: the high
 * half-word is a per-field write-enable mask, the low half-word the
 * data, so a field can be updated without a read-modify-write and
 * without disturbing neighbouring pins in the same register.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_pinctrl.h"
#include "rk3506_register.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register access (fall back if the arch headers do not provide these) */

/* Per-bank IOC base addresses.  These differ per bank; using the shared
 * IOC_GRF for bank1 (as the old rk3506_iomux.c did) is the M4.1 bug --
 * bank1 has its own IOC1 block.
 */

#define RK3506_IOC_BANK0   RK3506_IOC_PMU_ADDR   /* 0xFF950000 */
#define RK3506_IOC_BANK1   RK3506_IOC1_ADDR      /* 0xFF660000 */
#define RK3506_IOC_BANK23  RK3506_IOC_GRF_ADDR   /* 0xFF4D8000 (2,3,4) */

/* IOMUX: 4-bit function select per pin, 4 pins per 32-bit register.
 * Per-bank offset of the first IOMUX register of each 8-pin group
 * (rk3506_pin_banks[], PIN_BANK_IOMUX_FLAGS_OFFSET).
 */

#define IOMUX_OFF_BANK0_A  0x00u
#define IOMUX_OFF_BANK0_B  0x08u
#define IOMUX_OFF_BANK0_C  0x10u
#define IOMUX_OFF_BANK0_D  0x830u  /* 2-bit field, in GPIO0D_CON */
#define IOMUX_OFF_BANK1_A  0x20u
#define IOMUX_OFF_BANK2_A  0x40u
#define IOMUX_OFF_BANK3_A  0x60u
#define IOMUX_OFF_BANK4_A  0x80u   /* never written: bank4 mux no-op */

#define IOMUX_GROUP_STRIDE 0x08u   /* A->B->C->D within a bank */

/* DRIVE: 8-bit thermometer code per pin, 2 pins per register
 * (rk3506_calc_drv_reg_and_bit).  Special pins carry a plain 2-bit
 * level inside a combined register.
 */

#define DRV_OFF_BANK0_A    0x100u
#define DRV_OFF_BANK1      0x140u
#define DRV_OFF_BANK2      0x180u
#define DRV_OFF_BANK3      0x1c0u
#define DRV_OFF_BANK0_D    0x830u  /* 2-bit, bit 3  */
#define DRV_OFF_BANK4      0x840u  /* 2-bit, bit 10 */
#define DRV_BITS_PER_PIN   8u
#define DRV_PINS_PER_REG   2u

/* PULL: 2-bit per pin, 8 pins per register
 * (rk3506_calc_pull_reg_and_bit).
 */

#define PULL_OFF_BANK0_A   0x200u
#define PULL_OFF_BANK1     0x210u
#define PULL_OFF_BANK2     0x220u
#define PULL_OFF_BANK3     0x230u
#define PULL_OFF_BANK0_D   0x830u  /* 2-bit, bit 5  */
#define PULL_OFF_BANK4     0x840u  /* 2-bit, bit 13 */
#define PULL_BITS_PER_PIN  2u
#define PULL_PINS_PER_REG  8u

/* SCHMITT: 1-bit per pin, 8 pins per register
 * (rk3506_calc_schmitt_reg_and_bit).
 */

#define SMT_OFF_BANK0_A    0x400u
#define SMT_OFF_BANK1      0x410u
#define SMT_OFF_BANK2      0x420u
#define SMT_OFF_BANK3      0x430u
#define SMT_OFF_BANK0_D    0x830u  /* 2-bit, bit 9  */
#define SMT_OFF_BANK4      0x840u  /* 2-bit, bit 8  */

/* RM_IO routing matrix at GRF_PMU (0xFF910000).
 * FUNC_SEL is 7 bits; the high half-word is the write-enable mask.
 * (rockchip_set_rmio: data = 0x7f0000 | function, then mux = 7.)
 */

#define RMIO_FUNCSEL_MASK  0x7fu
#define RMIO_HIWORD        0x7f0000u

/* Number of GPIO banks and pins per bank */

#define RK3506_NR_BANKS    5u
#define RK3506_BANK_NPINS  32u

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_ioc_base
 *
 * Description:
 *   Return the IOC base address for a bank, or 0 for an invalid bank.
 *   This per-bank mapping is the single place that knows bank1 uses the
 *   dedicated IOC1 block (not the shared IOC_GRF).
 *
 ****************************************************************************/

static uintptr_t rk3506_ioc_base(uint8_t bank)
{
  switch (bank)
    {
      case 0:
        return RK3506_IOC_BANK0;

      case 1:
        return RK3506_IOC_BANK1;

      case 2:
      case 3:
      case 4:
        return RK3506_IOC_BANK23;

      default:
        return 0;
    }
}

/****************************************************************************
 * Name: rk3506_rmio_offset
 *
 * Description:
 *   Return the RM_IO channel register offset (from GRF_PMU) for a
 *   (bank, pin), or a negative errno if the pin has no RM_IO channel.
 *
 *   Mapping (struct RM_IO_REG, rk3506.h:1834-1867; rockchip_set_rmio,
 *   pinctrl-rk3506.c:29-45):
 *     bank0 pin 0..23  -> 0x80 + 4*pin   (GPIO0_A0..C7)
 *     bank1 pin 9..11  -> 0xbc + 4*pin   (GPIO1_B1,B2,B3  -> 0xE0,E4,E8)
 *     bank1 pin 18..19 -> 0xa4 + 4*pin   (GPIO1_C2,C3     -> 0xEC,F0)
 *     bank1 pin 25..27 -> 0x90 + 4*pin   (GPIO1_D1,D2,D3  -> 0xF4,F8,FC)
 *
 ****************************************************************************/

static int rk3506_rmio_reg_offset(uint8_t bank, uint8_t pin)
{
  if (bank == 0)
    {
      if (pin < 24)
        {
          return 0x80 + 0x4 * pin;
        }
    }
  else if (bank == 1)
    {
      if (pin >= 9 && pin <= 11)
        {
          return 0xbc + 0x4 * pin;
        }
      else if (pin >= 18 && pin <= 19)
        {
          return 0xa4 + 0x4 * pin;
        }
      else if (pin >= 25 && pin <= 27)
        {
          return 0x90 + 0x4 * pin;
        }
    }

  return -EINVAL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_pinctrl_rmio_offset
 ****************************************************************************/

int rk3506_pinctrl_rmio_offset(uint8_t bank, uint8_t pin)
{
  if (bank >= RK3506_NR_BANKS || pin >= RK3506_BANK_NPINS)
    {
      return -EINVAL;
    }

  return rk3506_rmio_reg_offset(bank, pin);
}

/****************************************************************************
 * Name: rk3506_pinctrl_get_mux
 ****************************************************************************/

int rk3506_pinctrl_get_mux(uint8_t bank, uint8_t pin)
{
  uintptr_t base;
  uint32_t  reg;
  uint32_t  bit;
  uint8_t   group;

  if (bank >= RK3506_NR_BANKS || pin >= RK3506_BANK_NPINS)
    {
      return -EINVAL;
    }

  if (bank == 4)
    {
      return 0;    /* bank4 is GPIO-only, mux is hard-wired */
    }

  base = rk3506_ioc_base(bank);
  if (base == 0)
    {
      return -EINVAL;
    }

  group = pin / 8u;

  if (bank == 0)
    {
      if (group < 3)
        {
          reg = IOMUX_OFF_BANK0_A + (uint32_t)group * IOMUX_GROUP_STRIDE;
          bit = (pin % 4u) * 4u;
          if ((pin % 8u) >= 4u)
            {
              reg += 0x4u;
            }
        }
      else
        {
          reg = IOMUX_OFF_BANK0_D;
          bit = 0u;
        }
    }
  else
    {
      static const uint8_t bank_base[4] =
        {
          0, IOMUX_OFF_BANK1_A, IOMUX_OFF_BANK2_A, IOMUX_OFF_BANK3_A
        };

      reg = bank_base[bank] + (uint32_t)group * IOMUX_GROUP_STRIDE;
      bit = (pin % 4u) * 4u;
      if ((pin % 8u) >= 4u)
        {
          reg += 0x4u;
        }
    }

  return (int)((getreg32(base + reg) >> bit) & 0xfu);
}

/****************************************************************************
 * Name: rk3506_pinctrl_set_mux
 ****************************************************************************/

int rk3506_pinctrl_set_mux(uint8_t bank, uint8_t pin, uint8_t func)
{
  uintptr_t base;
  uint32_t  reg;
  uint32_t  bit;
  uint32_t  mask;
  uint32_t  data;
  uint8_t   group;
  int       rmio_off;

  if (bank >= RK3506_NR_BANKS || pin >= RK3506_BANK_NPINS)
    {
      return -EINVAL;
    }

  /* An RM_IO-matrix pin has a function number that exceeds the 4-bit
   * IOMUX field.  Program the routing channel with (func - 15) and then
   * force the local IOMUX nibble to the matrix path (7), exactly as
   * rockchip_set_rmio() does.
   */

  if (func > 0xfu)
    {
      rmio_off = rk3506_rmio_reg_offset(bank, pin);
      if (rmio_off < 0)
        {
          return -EINVAL;
        }

      data = RMIO_HIWORD | ((func - 0xfu) & RMIO_FUNCSEL_MASK);
      putreg32(data, RK3506_GRF_PMU_ADDR + (uint32_t)rmio_off);
      func = RK3506_RMIO_IOMUX_FUNC;
    }

  /* bank4 pins are GPIO-only; their mux write is a no-op in the SDK
   * (rk3506_set_mux:84-85 returns before touching any register).
   */

  if (bank == 4)
    {
      return OK;
    }

  base = rk3506_ioc_base(bank);
  if (base == 0)
    {
      return -EINVAL;
    }

  /* Per-bank IOMUX group offset (rk3506_pin_banks[]).  group = pin/8. */

  group = pin / 8u;

  if (bank == 0)
    {
      /* bank0: A@0x00 B@0x08 C@0x10, D is a 2-bit field in GPIO0D_CON */

      if (group < 3)
        {
          reg  = IOMUX_OFF_BANK0_A + (uint32_t)group * IOMUX_GROUP_STRIDE;
          bit  = (pin % 4u) * 4u;
          mask = 0xfu;
          if ((pin % 8u) >= 4u)
            {
              reg += 0x4u;
            }
        }
      else
        {
          /* GPIO0D: 2-bit field at GPIO0D_CON(0x830) bit 0 (recalced) */

          reg  = IOMUX_OFF_BANK0_D;
          bit  = 0u;
          mask = 0x3u;
        }
    }
  else
    {
      /* bank1/2/3: A base per bank, groups B/C/D at +0x08 stride */

      static const uint8_t bank_base[4] =
        {
          0, IOMUX_OFF_BANK1_A, IOMUX_OFF_BANK2_A, IOMUX_OFF_BANK3_A
        };

      reg  = bank_base[bank] + (uint32_t)group * IOMUX_GROUP_STRIDE;
      bit  = (pin % 4u) * 4u;
      mask = 0xfu;
      if ((pin % 8u) >= 4u)
        {
          reg += 0x4u;
        }
    }

  data = (mask << (bit + 16u)) | ((func & mask) << bit);
  putreg32(data, base + reg);

  return OK;
}

/****************************************************************************
 * Name: rk3506_pinctrl_set_pull
 ****************************************************************************/

int rk3506_pinctrl_set_pull(uint8_t bank, uint8_t pin, uint8_t pull)
{
  uintptr_t base;
  uint32_t  reg;
  uint32_t  bit;
  uint32_t  data;
  uint8_t   val = pull;

  if (bank >= RK3506_NR_BANKS || pin >= RK3506_BANK_NPINS ||
      pull > RK3506_PULL_BUS_HOLD)
    {
      return -EINVAL;
    }

  base = rk3506_ioc_base(bank);
  if (base == 0)
    {
      return -EINVAL;
    }

  /* Special pins: GPIO0_D0 (pin 24) and GPIO4 use the combined
   * register with a 1V8 pull type (type 1: 0=disable 1=down 2=disable
   * 3=up).  Translate the logical pull to the type-1 encoding.
   */

  if ((bank == 0 && pin == 24) || bank == 4)
    {
      static const uint8_t pull_type1[4] = { 0, 3, 1, 0 };

      val = pull_type1[pull];
      reg = (bank == 0) ? PULL_OFF_BANK0_D : PULL_OFF_BANK4;
      bit = (bank == 0) ? 5u : 13u;

      data = (0x3u << (bit + 16u)) | ((uint32_t)val << bit);
      putreg32(data, base + reg);
      return OK;
    }

  /* Default pull type 0: 0=disable 1=up 2=down 3=bus-hold, which is
   * exactly the logical RK3506_PULL_* numbering, so no translation.
   */

  switch (bank)
    {
      case 0:
        reg = PULL_OFF_BANK0_A;
        break;

      case 1:
        reg = PULL_OFF_BANK1;
        break;

      case 2:
        reg = PULL_OFF_BANK2;
        break;

      default:
        reg = PULL_OFF_BANK3;
        break;
    }

  reg += (pin / PULL_PINS_PER_REG) * 4u;
  bit  = (pin % PULL_PINS_PER_REG) * PULL_BITS_PER_PIN;

  data = (0x3u << (bit + 16u)) | ((uint32_t)val << bit);
  putreg32(data, base + reg);

  return OK;
}

/****************************************************************************
 * Name: rk3506_pinctrl_set_drive
 ****************************************************************************/

int rk3506_pinctrl_set_drive(uint8_t bank, uint8_t pin, uint8_t level)
{
  uintptr_t base;
  uint32_t  reg;
  uint32_t  bit;
  uint32_t  data;
  uint32_t  code;

  if (bank >= RK3506_NR_BANKS || pin >= RK3506_BANK_NPINS || level > 7u)
    {
      return -EINVAL;
    }

  base = rk3506_ioc_base(bank);
  if (base == 0)
    {
      return -EINVAL;
    }

  /* Special pins carry a plain 2-bit level (rk3506_set_drive:206-209). */

  if ((bank == 0 && pin == 24) || bank == 4)
    {
      reg   = (bank == 0) ? DRV_OFF_BANK0_D : DRV_OFF_BANK4;
      bit   = (bank == 0) ? 3u : 10u;
      code  = level;

      data = ((0x3u) << (bit + 16u)) | (code << bit);
      putreg32(data, base + reg);
      return OK;
    }

  switch (bank)
    {
      case 0:
        reg = DRV_OFF_BANK0_A;
        break;

      case 1:
        reg = DRV_OFF_BANK1;
        break;

      case 2:
        reg = DRV_OFF_BANK2;
        break;

      default:
        reg = DRV_OFF_BANK3;
        break;
    }

  /* Thermometer code: rk3506_set_drive:203-204 starts from ret=1 and
   * shifts in one 1-bit per level, giving (1 << (level + 1)) - 1:
   * level 0->0x01, 1->0x03, 2->0x07 ... 7->0xff.
   */

  reg += (pin / DRV_PINS_PER_REG) * 4u;
  bit  = (pin % DRV_PINS_PER_REG) * DRV_BITS_PER_PIN;
  code = (1u << (level + 1u)) - 1u;

  data = (0xffu << (bit + 16u)) | (code << bit);
  putreg32(data, base + reg);

  return OK;
}

/****************************************************************************
 * Name: rk3506_pinctrl_set_schmitt
 ****************************************************************************/

int rk3506_pinctrl_set_schmitt(uint8_t bank, uint8_t pin, bool enable)
{
  uintptr_t base;
  uint32_t  reg;
  uint32_t  bit;
  uint32_t  data;

  if (bank >= RK3506_NR_BANKS || pin >= RK3506_BANK_NPINS)
    {
      return -EINVAL;
    }

  base = rk3506_ioc_base(bank);
  if (base == 0)
    {
      return -EINVAL;
    }

  /* Special pins use a 2-bit field (rk3506_set_schmitt:441-444). */

  if ((bank == 0 && pin == 24) || bank == 4)
    {
      reg  = (bank == 0) ? SMT_OFF_BANK0_D : SMT_OFF_BANK4;
      bit  = (bank == 0) ? 9u : 8u;
      data = (0x3u << (bit + 16u)) |
             ((enable ? 0x3u : 0u) << bit);
      putreg32(data, base + reg);
      return OK;
    }

  switch (bank)
    {
      case 0:
        reg = SMT_OFF_BANK0_A;
        break;

      case 1:
        reg = SMT_OFF_BANK1;
        break;

      case 2:
        reg = SMT_OFF_BANK2;
        break;

      default:
        reg = SMT_OFF_BANK3;
        break;
    }

  reg += (pin / 8u) * 4u;
  bit  = (pin % 8u);

  data = (0x1u << (bit + 16u)) | ((enable ? 1u : 0u) << bit);
  putreg32(data, base + reg);

  return OK;
}
