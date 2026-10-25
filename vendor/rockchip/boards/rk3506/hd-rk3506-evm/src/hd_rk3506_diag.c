/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_diag.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * One-shot bringup diagnostic dump.  Reads back the RM_IO routing matrix,
 * the GPIO0 clock gates, the GPIO0 pad IOMUX nibbles and the USB-PHY
 * suspend bit so we can see whether the unified pinctrl/gpio/cru writes
 * actually landed.
 *
 * This file is self-contained and only built when the board bringup
 * references rk3506_bringup_diag().
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <debug.h>
#include <inttypes.h>

#include "rk3506_register.h"
#include "hardware/rk3506_memorymap.h"

/* Register bases used here */

#define PMU_GRF        RK3506_GRF_PMU_ADDR   /* 0xff910000 RM_IO matrix   */
#define IOC_PMU        RK3506_IOC_PMU_ADDR   /* 0xff950000 GPIO0 iomux    */
#define PMU_CRU        RK3506_PMU_CRU_ADDR   /* 0xff9b0000 PMU CRU        */
#define GRF            RK3506_GRF_ADDR       /* 0xff288000 USB phy_sus    */

#define PMUCRU_CLKGATE(n)  (PMU_CRU + 0x800u + 4u * (n))

/* RM_IO channel N route-select register: GRF_PMU + 0x80 + 4*N */

#define RMIO_SEL(n)        (PMU_GRF + 0x80u + 4u * (n))

void rk3506_bringup_diag(void)
{
  _err("=== DIAG: RM_IO route-select (GRF_PMU) ===\n");
  _err("  ch1  MOSI  SEL @ff910084 = 0x%08" PRIx32 " (want 68=83-15)\n",
       getreg32(RMIO_SEL(1)));
  _err("  ch3  CS    SEL @ff91008c = 0x%08" PRIx32 " (GPIO, want 0/gpio)\n",
       getreg32(RMIO_SEL(3)));
  _err("  ch7  CLK   SEL @ff91009c = 0x%08" PRIx32 " (want 67=82-15)\n",
       getreg32(RMIO_SEL(7)));
  _err("  ch8  RST   SEL @ff9100a0 = 0x%08" PRIx32 "\n", getreg32(RMIO_SEL(8)));
  _err("  ch11 DC    SEL @ff9100ac = 0x%08" PRIx32 "\n", getreg32(RMIO_SEL(11)));
  _err("  ch12 BL    SEL @ff9100b0 = 0x%08" PRIx32 " (GPIO, want 0)\n",
       getreg32(RMIO_SEL(12)));

  _err("=== DIAG: clock gates (PMU CRU) ===\n");
  _err("  CLKGATE0 @ff9b0800 = 0x%08" PRIx32
       " (bit8 PCLK_GPIO0, want 0=on)\n",
       getreg32(PMUCRU_CLKGATE(0)));
  _err("  CLKGATE1 @ff9b0804 = 0x%08" PRIx32 "\n", getreg32(PMUCRU_CLKGATE(1)));

  _err("=== DIAG: GPIO0 pad IOMUX (IOC_PMU) ===\n");
  _err("  GPIO0A_CON_LO @ff950000 = 0x%08" PRIx32 " (A0-A3; A1 want 7)\n",
       getreg32(IOC_PMU + 0x00));
  _err("  GPIO0A_CON_HI @ff950004 = 0x%08" PRIx32 " (A4-A7; A7 want 7)\n",
       getreg32(IOC_PMU + 0x04));
  _err("  GPIO0B_CON @ff950008 = 0x%08" PRIx32 " (B0/B3 nibbles, GPIO=0)\n",
       getreg32(IOC_PMU + 0x08));
  _err("  GPIO0B_CON_HI @ff95000c = 0x%08" PRIx32
       " (B4 nibble0, want 0 = GPIO for BL)\n",
       getreg32(IOC_PMU + 0x0c));

  /* GPIO0 controller (base 0xff940000): does the SOFTWARE GPIO actually
   * drive CS(A3)/RST(B0)/DC(B3)?  SWPORT_DDR_L/H must have the output
   * bits set; EXT_PORT_L reads back the live pad level.  GPIO0 is a v2
   * controller: 32 pins across DR_L(0x00,A0-A15) + DR_H(0x04,A16-A31),
   * DDR_L(0x08)/DDR_H(0x0c), EXT_PORT_L(0x70)/H(0x74). */

  _err("=== DIAG: GPIO0 controller (0xff940000) ===\n");
  _err("  SWPORT_DR_L  +0x00 = 0x%08" PRIx32 "\n", getreg32(0xff940000 + 0x00));
  _err("  SWPORT_DDR_L +0x08 = 0x%08" PRIx32
       " (A3/B0/B3 output bits want 1)\n", getreg32(0xff940000 + 0x08));
  _err("  EXT_PORT_L   +0x70 = 0x%08" PRIx32
       " (live pad levels)\n", getreg32(0xff940000 + 0x70));

  /* SPI0 controller (base per rk3506_spi.c): is it enabled, what mode,
   * and is SER set so SCLK is generated for CSN0.  These registers decide
   * whether SCLK can ever toggle on GPIO0_A7. */

  _err("=== DIAG: SPI0 controller ===\n");
  {
    extern void rk3506_spi0_diag(void);
    rk3506_spi0_diag();
  }

  _err("=== DIAG: USB PHY suspend (GRF) ===\n");
  _err("  SOC_CON24 @ff288060 = 0x%08" PRIx32 " (OTG0 phy_sus[8:0])\n",
       getreg32(GRF + 0x60));
  _err("  SOC_CON28 @ff288070 = 0x%08" PRIx32
       " (OTG1 phy_sus[8:0], want 0=awake)\n",
       getreg32(GRF + 0x70));

  /* DWC2 OTG1 core registers (base 0xff780000).  GUSBCFG tells us whether
   * the PHY interface came up in the right mode (PHYSEL must be 0 for the
   * 8-bit UTMI+ HS PHY); GINTSTS/GINTMSK show whether the IRQ line is
   * alive; HPRT[0]=PCSTS connection detect is the live "U盘插了吗" bit.
   */

  _err("=== DIAG: DWC2 OTG1 core (0xff780000) ===\n");
  _err("  GOTGCTL  +0x000 = 0x%08" PRIx32 "\n", getreg32(0xff780000 + 0x000));
  _err("  GUSBCFG  +0x00c = 0x%08" PRIx32
       " (bit6 PHYSEL want 0=UTMI, TRDT[13:10] want 9)\n",
       getreg32(0xff780000 + 0x00c));
  _err("  GINTSTS  +0x014 = 0x%08" PRIx32 " (bit1 CURMOD want 1=host)\n",
       getreg32(0xff780000 + 0x014));
  _err("  GINTMSK  +0x018 = 0x%08" PRIx32 "\n", getreg32(0xff780000 + 0x018));
  _err("  GHWCFG2  +0x048 = 0x%08" PRIx32 "\n", getreg32(0xff780000 + 0x048));
  _err("  HPRT     +0x440 = 0x%08" PRIx32
       " (bit0 PCSTS: 1=device connected)\n", getreg32(0xff780000 + 0x440));

  _err("=== DIAG done ===\n");
}
