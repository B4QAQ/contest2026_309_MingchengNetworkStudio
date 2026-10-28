/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_cru.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal Clock & Reset Unit (CRU) support for RK3506G2.
 *
 * Only the SPI1 clock ungating needed to bring the on-board SPI NAND up
 * is implemented.  All register offsets / bit positions are taken from
 * the Linux Rockchip clock driver (drivers/clk/rockchip/clk-rk3506.c),
 * which encodes them as:
 *
 *   GATE(PCLK_SPI1, ..., CLKGATE_CON(12), bit 12, GFLAGS)
 *   COMPOSITE(CLK_SPI1, ..., gate CLKGATE_CON(12), bit 13, GFLAGS)
 *
 * with CLKGATE_CON(x) = CRU + x*4 + 0x800  and
 *      GFLAGS = CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE.
 *
 * HIWORD_MASK means a write takes an action bitmask in the high
 * half-word and the new values in the low half-word.
 * SET_TO_DISABLE means the gate is ENABLED by writing a 0 data bit.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <errno.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_cru.h"
#include "rk3506_register.h"

/* CRU register access (fall back to a volatile store if the arch
 * headers do not already provide putreg32).
 */

/* Register bank offsets from the CRU base are defined once, publicly,
 * in rk3506_cru.h (RK3506_CRU_CLKSEL_CON / RK3506_CRU_CLKGATE_CON /
 * RK3506_CRU_SOFTRST_CON and the PMU-CRU variants); this file uses
 * those names directly.
 */

/* SPI1 gate bits inside CLKGATE_CON(12) (from the Linux clk tables) */

#define CLKGATE12_PCLK_SPI1  (1u << 12)   /* APB register interface   */
#define CLKGATE12_CLK_SPI1   (1u << 13)   /* functional serial clock  */

/* CLK_SPI1 mux/divider inside CLKSEL_CON(34) (COMPOSITE in clk-rk3506.c):
 *   mux  bits 14..15 : parent select, 0 = xin24m_gate (24 MHz)
 *   div  bits 10..13 : serial-clock divider, 0 = /1
 * We force the parent to the 24 MHz crystal with /1 so the SPI baud
 * divider (sclk = spiclk / BAUDR) runs from a known 24 MHz source.
 */

#define CLKSEL34_SPI1_MUX_SHIFT   14
#define CLKSEL34_SPI1_MUX_MASK    (0x3u << CLKSEL34_SPI1_MUX_SHIFT)
#define CLKSEL34_SPI1_DIV_SHIFT   10
#define CLKSEL34_SPI1_DIV_MASK    (0xfu << CLKSEL34_SPI1_DIV_SHIFT)
#define CLKSEL34_SPI1_XIN24M      (0x0u << CLKSEL34_SPI1_MUX_SHIFT)
#define CLKSEL34_SPI1_DIV1        (0x0u << CLKSEL34_SPI1_DIV_SHIFT)

/* SPI0 shares CLKGATE_CON(12) / CLKSEL_CON(34) with SPI1 (clk-rk3506.c):
 *   PCLK_SPI0 : GATE      CLKGATE_CON(12), bit 10
 *   CLK_SPI0  : COMPOSITE gate bit 11; mux bits 8..9 (0 = xin24m_gate),
 *               divider bits 4..7 (0 = /1)
 */

#define CLKGATE12_PCLK_SPI0  (1u << 10)
#define CLKGATE12_CLK_SPI0   (1u << 11)

#define CLKSEL34_SPI0_MUX_SHIFT   8
#define CLKSEL34_SPI0_DIV_SHIFT   4
#define CLKSEL34_SPI0_MUX_MASK    (0x3u << CLKSEL34_SPI0_MUX_SHIFT)
#define CLKSEL34_SPI0_DIV_MASK    (0xfu << CLKSEL34_SPI0_DIV_SHIFT)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_cru_gate
 *
 * Description:
 *   Generic main-CRU clock-gate primitive (see rk3506_cru.h).  A
 *   single HIWORD_UPDATE store touches only the requested gate bits.
 *
 ****************************************************************************/

void rk3506_cru_gate(uint8_t con, uint32_t mask, bool enable)
{
  /* CLK_GATE_SET_TO_DISABLE: data bit 1 disables, 0 enables.  The
   * high half-word is the per-bit write-enable mask.
   */

  uint32_t data = enable ? 0u : mask;

  putreg32((mask << 16) | data, RK3506_CRU_CLKGATE_CON(con));
}

/****************************************************************************
 * Name: rk3506_cru_reset
 *
 * Description:
 *   Generic main-CRU soft-reset primitive (see rk3506_cru.h).  Data
 *   bit 1 asserts the reset, 0 releases it.
 *
 ****************************************************************************/

void rk3506_cru_reset(uint8_t con, uint32_t mask, bool assert)
{
  uint32_t data = assert ? mask : 0u;

  putreg32((mask << 16) | data, RK3506_CRU_SOFTRST_CON(con));
}

/****************************************************************************
 * Name: rk3506_spi0_clock_init
 *
 * Description:
 *   Ungate the SPI0 PCLK and functional clock and force CLK_SPI0 to the
 *   24 MHz crystal with /1 so the DesignWare SSI at 0xFF120000 has a known
 *   bit-rate source.  HIWORD_UPDATE writes only touch the SPI0 gate and
 *   mux/div fields; every other clock in the two registers is untouched.
 *
 ****************************************************************************/

void rk3506_spi0_clock_init(void)
{
  uint32_t gates = CLKGATE12_PCLK_SPI0 | CLKGATE12_CLK_SPI0;

  /* Parent = xin24m (mux 0), serial divider /1 (div 0). */

  uint32_t sel_hi = (CLKSEL34_SPI0_MUX_MASK | CLKSEL34_SPI0_DIV_MASK) << 16;

  putreg32(sel_hi, RK3506_CRU_CLKSEL_CON(34));
  putreg32(gates << 16, RK3506_CRU_CLKGATE_CON(12));
}

/****************************************************************************
 * Name: rk3506_spi1_clock_init
 *
 * Description:
 *   Ungate the SPI1 PCLK (APB) and functional clock.  Only the two gate
 *   bits are written; the HIWORD_UPDATE mask leaves every other clock
 *   in CLKGATE_CON(12) untouched.
 *
 ****************************************************************************/

void rk3506_spi1_clock_init(void)
{
  uint32_t gates = CLKGATE12_PCLK_SPI1 | CLKGATE12_CLK_SPI1;
  uint32_t reg   = RK3506_CRU_CLKGATE_CON(12);

  /* HIWORD_UPDATE: high half-word = write-enable mask, low half-word =
   * data (0 = enable for SET_TO_DISABLE gates).
   */

  putreg32(gates << 16, reg);
}

/****************************************************************************
 * Name: rk3506_i2c_clock_init
 *
 * Description:
 *   Ungate one I2C controller's PCLK (register interface) and CLK_I2Cx
 *   (functional clock), per the Linux clock tables
 *   (drivers/clk/rockchip/clk-rk3506.c, all GFLAGS = HIWORD_MASK |
 *   SET_TO_DISABLE, i.e. write the WE mask with a 0 data bit to open):
 *
 *     I2C0 : PCLK_I2C0 CLKGATE_CON(11) bit 14, CLK_I2C0 bit 15
 *     I2C1 : PCLK_I2C1 CLKGATE_CON(12) bit 0,  CLK_I2C1 bit 1
 *     I2C2 : PCLK_I2C2 CLKGATE_CON(12) bit 2,  CLK_I2C2 bit 3
 *
 *   The CLK_I2Cx mux/divider (CLKSEL_CON(32)/(33)) is left at its reset
 *   parent (xin24m); the i2c-rk3x driver programs its own SCL divider
 *   from the resulting input clock, so ungating the two gates is all
 *   that is needed for register access and transfers.  Previously the
 *   driver relied on U-Boot having ungated these, which broke when the
 *   boot chain did not.
 *
 * Input Parameters:
 *   bus - I2C controller number, 0..2
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL for an out-of-range bus.
 *
 ****************************************************************************/

#define CLKGATE11_PCLK_I2C0  (1u << 14)
#define CLKGATE11_CLK_I2C0   (1u << 15)
#define CLKGATE12_PCLK_I2C1  (1u << 0)
#define CLKGATE12_CLK_I2C1   (1u << 1)
#define CLKGATE12_PCLK_I2C2  (1u << 2)
#define CLKGATE12_CLK_I2C2   (1u << 3)

int rk3506_i2c_clock_init(uint8_t bus)
{
  uint32_t gates;
  uint32_t reg;

  switch (bus)
    {
      case 0:
        gates = CLKGATE11_PCLK_I2C0 | CLKGATE11_CLK_I2C0;
        reg   = RK3506_CRU_CLKGATE_CON(11);
        break;

      case 1:
        gates = CLKGATE12_PCLK_I2C1 | CLKGATE12_CLK_I2C1;
        reg   = RK3506_CRU_CLKGATE_CON(12);
        break;

      case 2:
        gates = CLKGATE12_PCLK_I2C2 | CLKGATE12_CLK_I2C2;
        reg   = RK3506_CRU_CLKGATE_CON(12);
        break;

      default:
        return -EINVAL;
    }

  putreg32(gates << 16, reg);

  return OK;
}

/****************************************************************************
 * Name: rk3506_fspi_clock_init
 *
 * Description:
 *   Bring up the FSPI serial-flash controller clock (the on-board SPI
 *   NAND is wired to FSPI @0xFF488000, flash@0).  From the Linux clock
 *   tables (clk-rk3506.c):
 *
 *     HCLK_FSPI  : GATE,      CLKGATE_CON(17), bit 8  (AHB reg access)
 *     SCLK_FSPI  : COMPOSITE, CLKSEL_CON(50) mux[6:5] / div[4:0],
 *                            gate CLKGATE_CON(17), bit 9
 *
 *   The composite mux parent table is
 *     { xin24m_gate, clk_gpll_gate, clk_v0pll_gate, clk_v1pll_gate }
 *
 *   FSPI v9 (rockchip,fspi) uses the "sclk x2" clock path by default
 *   (no rockchip,sclk-x2-bypass in DTS): the controller internally
 *   halves its source clock to derive sclk_out, so SCLK_FSPI must be
 *   2x the desired SPI bit rate.  The Linux kernel driver
 *   (drivers/spi/spi-rockchip.c) sets exactly this:
 *
 *     clk_set_rate(rs->spiclk, 2 * xfer->speed_hz);
 *
 *   For a 24 MHz sclk_out we program SCLK_FSPI = 48 MHz from the GPLL
 *   (1200 MHz, assigned-clock-rates = <1200000000> in the U-Boot DTS)
 *   with div = 1200/48 - 1 = 24.  Both HCLK and SCLK gates are then
 *   ungated.
 *
 ****************************************************************************/

#define CLKGATE17_HCLK_FSPI   (1u << 8)    /* AHB register interface  */
#define CLKGATE17_SCLK_FSPI   (1u << 9)    /* functional serial clock */

#define CLKSEL50_FSPI_MUX_SHIFT   5
#define CLKSEL50_FSPI_MUX_MASK    (0x3u << CLKSEL50_FSPI_MUX_SHIFT)  /* [6:5] */
#define CLKSEL50_FSPI_DIV_MASK    (0x1fu << 0)                       /* [4:0] */

void rk3506_fspi_clock_init(void)
{
  uint32_t selset = (CLKSEL50_FSPI_MUX_MASK | CLKSEL50_FSPI_DIV_MASK);
  uint32_t selval;
  uint32_t gates  = CLKGATE17_HCLK_FSPI | CLKGATE17_SCLK_FSPI;

  /* For FSPI v9 (sclk-x2) the SPI bit rate is SCLK_FSPI / 2.  For a
   * 24 MHz sclk_out we need SCLK_FSPI = 48 MHz.  Source it from GPLL
   * (1200 MHz, left running by U-Boot at the DTS-assigned rate) with
   * mux index 1 (clk_gpll_gate) and div = 1200/48 - 1 = 24.
   */

  selval = (1u << CLKSEL50_FSPI_MUX_SHIFT) | 24u;

  putreg32((selset << 16) | selval, RK3506_CRU_CLKSEL_CON(50));

  /* Ungate HCLK_FSPI and SCLK_FSPI (gate bit 0 = enable for
   * CLK_GATE_SET_TO_DISABLE gates).
   */

  putreg32(gates << 16, RK3506_CRU_CLKGATE_CON(17));
}

/****************************************************************************
 * Name: rk3506_gmac0_clock_init
 *
 * Description:
 *   Bring up the GMAC0 controller clock and deassert its reset, so the
 *   on-board RTL8211F PHY (RMII) can be probed over MDIO.  Register
 *   positions are taken from the Linux kernel SDK
 *   drivers/clk/rockchip/clk-rk3506.c and the reset IDs from
 *   include/dt-bindings/clock/rockchip,rk3506-cru.h.
 *
 *     CLK_MAC_ROOT    : COMPOSITE_NOMUX, div[11:7] CLKSEL_CON(50),
 *                       gate CLKGATE_CON(17) bit 15, parent gpll
 *     CLK_MAC_PTP_ROOT: COMPOSITE,     mux[6:5] / div[4:0] CLKSEL_CON(55),
 *                       gate CLKGATE_CON(19) bit 9
 *     ACLK_MAC0       : GATE,          CLKGATE_CON(17) bit 11
 *     PCLK_MAC0       : GATE,          CLKGATE_CON(17) bit 13
 *     CLK_MAC0        : GATE,          CLKGATE_CON(18) bit 0
 *     CLK_MAC0_PTP    : GATE,          CLKGATE_CON(19) bit 10
 *     SRST_A_MAC0     : reset ID 283, SOFTRST_CON(17) bit 11
 *
 *   Divider for CLK_MAC_ROOT defaults to /2 (div field 1) so the MAC
 *   rx_clk / tx_clk run at 150 MHz nominal (GPLL 1.2 GHz / 2) - the
 *   RMII bus itself is 50 MHz regardless, the MAC core clock is
 *   separate.  We leave the default divider for now and only ungate.
 *
 *   For the PTP mux we force the parent to xin24m (mux 0) with div=0
 *   so PTP is functional even before a final reference clock is
 *   decided.
 *
 ****************************************************************************/

#define CLKGATE17_ACLK_HSPERI_ROOT   (1u << 0)   /* parent of PCLK_HSPERI */
#define CLKGATE17_HCLK_HSPERI_ROOT   (1u << 1)   /* parent of PCLK_HSPERI */
#define CLKGATE17_PCLK_HSPERI_ROOT   (1u << 2)   /* parent of PCLK_MAC0  */
#define CLKGATE17_ACLK_MAC0          (1u << 11)
#define CLKGATE17_PCLK_MAC0          (1u << 13)
#define CLKGATE17_CLK_MAC_ROOT       (1u << 15)

#define CLKGATE18_CLK_MAC0           (1u << 0)

#define CLKGATE19_CLK_MAC_PTP_ROOT   (1u << 9)
#define CLKGATE19_CLK_MAC0_PTP       (1u << 10)

#define CLKSEL55_PTP_MUX_SHIFT 5
#define CLKSEL55_PTP_MUX_MASK  (0x3u << CLKSEL55_PTP_MUX_SHIFT)
#define CLKSEL55_PTP_DIV_MASK  (0x1fu << 0)
#define CLKSEL55_PTP_XIN24M    (0x0u << CLKSEL55_PTP_MUX_SHIFT)
#define CLKSEL55_PTP_DIV1      (0x0u)

#define SOFTRST17_SRST_A_MAC0  (1u << 11)

/* CLK_MAC_ROOT divider inside CLKSEL_CON(50), bits[11:7] (from the
 * HAL header rk3506_cru.h: CLK_MAC_ROOT_DIV = 0x05070032 = reg 0x32=50,
 * shift 7, width 5).  clk_mac_root feeds CLK_MAC0 which is the MAC
 * core / RMII reference clock (hal_bsp.c: clkID50M = CLK_MAC).  With
 * GPLL at 1200 MHz a divisor of 24 gives exactly 50 MHz - matching
 * the RMII reference requirement (clkID50M).
 */

#define CLKSEL50_MAC_ROOT_DIV_SHIFT   7
#define CLKSEL50_MAC_ROOT_DIV_MASK    (0x1fu << CLKSEL50_MAC_ROOT_DIV_SHIFT)
#define CLKSEL50_MAC_ROOT_DIV24       (23u << CLKSEL50_MAC_ROOT_DIV_SHIFT)

void rk3506_gmac0_clock_init(void)
{
  uint32_t gates;

  /* Select PTP parent xin24m / 1, then ungate the parent + leaf clocks. */

  putreg32(((CLKSEL55_PTP_MUX_MASK | CLKSEL55_PTP_DIV_MASK) << 16) |
           (CLKSEL55_PTP_XIN24M | CLKSEL55_PTP_DIV1),
           RK3506_CRU_CLKSEL_CON(55));

  /* Set CLK_MAC_ROOT = GPLL / 24 = 50 MHz.  This is the RMII MAC core
   * reference clock (hal_bsp.c clkID50M = CLK_MAC); without it the MAC
   * clock domain is dead and the DMA software reset never clears.
   * HIWORD write touches ONLY the div field bits[11:7] so the FSPI
   * mux/div fields in the same register (bits[6:0]) are preserved.
   */

  putreg32((CLKSEL50_MAC_ROOT_DIV_MASK << 16) | CLKSEL50_MAC_ROOT_DIV24,
           RK3506_CRU_CLKSEL_CON(50));

  gates = CLKGATE19_CLK_MAC_PTP_ROOT | CLKGATE19_CLK_MAC0_PTP |
          CLKGATE18_CLK_MAC0         |
          CLKGATE17_CLK_MAC_ROOT     |
          CLKGATE17_ACLK_MAC0       |
          CLKGATE17_PCLK_MAC0       |
          CLKGATE17_PCLK_HSPERI_ROOT|
          CLKGATE17_HCLK_HSPERI_ROOT|
          CLKGATE17_ACLK_HSPERI_ROOT;

  putreg32(gates << 16,
            (uint32_t)RK3506_CRU_CLKGATE_CON(17));
  putreg32((CLKGATE18_CLK_MAC0) << 16,
            (uint32_t)RK3506_CRU_CLKGATE_CON(18));
  putreg32(((CLKGATE19_CLK_MAC_PTP_ROOT | CLKGATE19_CLK_MAC0_PTP) << 16),
            (uint32_t)RK3506_CRU_CLKGATE_CON(19));

  /* Deassert SRST_A_MAC0.  Some Rockchip SOFTRST registers require a
   * pulse (assert then deassert) for the reset to actually release.
   * First assert (write 1 to the bit), then deassert (write 0).
   */

  putreg32(SOFTRST17_SRST_A_MAC0 << 16 | SOFTRST17_SRST_A_MAC0,
           RK3506_CRU_SOFTRST_CON(17));
  putreg32(SOFTRST17_SRST_A_MAC0 << 16, RK3506_CRU_SOFTRST_CON(17));
}

/****************************************************************************
 * Name: rk3506_pwm_clock_init
 *
 * Description:
 *   Ungate the PWM0 clocks (PMU CRU domain, per clk-rk3506.c):
 *
 *     PCLK_PWM0 : GATE, RK3506_PMUCRU_CLKGATE_CON(0), bit 15 (bus access)
 *     CLK_PWM0  : COMPOSITE_NOMUX, parent clk_gpll_div_100m (100 MHz),
 *                 div PMU_CLKSEL_CON(0) [9:6] left at /1 (reset 0),
 *                 gate RK3506_PMUCRU_CLKGATE_CON(1), bit 0
 *
 *   PMU CRU registers are at RK3506_PMU_CRU_ADDR (0xFF9B0000) +
 *   CLKSEL 0x300 / CLKGATE 0x800, HIWORD write-enable style.  The
 *   layout macros live in rk3506_cru.h (RK3506_PMUCRU_CLKGATE_CON).
 *
 ****************************************************************************/

#define PMU_CLKGATE0_PCLK_PWM0   (1u << 15)
#define PMU_CLKGATE1_CLK_PWM0    (1u << 0)

void rk3506_pwm_clock_init(void)
{
  /* Ungate PCLK_PWM0 (gate con 0, bit 15) and CLK_PWM0 (gate con 1,
   * bit 0).  The clock divider stays at the reset default (/1), so
   * the PWM counter runs at 100 MHz.
   */

  putreg32(PMU_CLKGATE0_PCLK_PWM0 << 16, RK3506_PMUCRU_CLKGATE_CON(0));
  putreg32(PMU_CLKGATE1_CLK_PWM0 << 16, RK3506_PMUCRU_CLKGATE_CON(1));
}

/****************************************************************************
 * Name: rk3506_gpio0_clock_init
 *
 * Description:
 *   Ungate PCLK_GPIO0 (PMU CRU domain, clk-rk3506.c:
 *   GATE(PCLK_GPIO0, ..., RK3506_PMUCRU_CLKGATE_CON(0), bit 8)).  GPIO0 is the
 *   always-on/PMU GPIO bank backing the RM_IO matrix pads; without this
 *   bus clock writes to its DR/DDR registers are silently ignored (reads
 *   back 0), so RST/DC GPIOs never drive.  The separate PCLK_GPIO0_IOC
 *   (bit 7) feeds the IOC mux and is already enabled.
 *
 ****************************************************************************/

#define PMU_CLKGATE0_PCLK_GPIO0  (1u << 8)
#define PMU_CLKGATE0_DBCLK_GPIO0 (1u << 9)

void rk3506_gpio0_clock_init(void)
{
  putreg32(PMU_CLKGATE0_PCLK_GPIO0 << 16, RK3506_PMUCRU_CLKGATE_CON(0));
}

/****************************************************************************
 * Name: rk3506_gpio_clock_enable
 *
 * Description:
 *   Ungate one GPIO bank's PCLK (register interface) and DBCLK
 *   (debounce / interrupt sampling clock), per the Linux clock tables
 *   (drivers/clk/rockchip/clk-rk3506.c, all GFLAGS = HIWORD_MASK |
 *   SET_TO_DISABLE, i.e. write the WE mask with a 0 data bit to open):
 *
 *     bank0 : PCLK_GPIO0  RK3506_PMUCRU_CLKGATE_CON(0) bit 8
 *             DBCLK_GPIO0 RK3506_PMUCRU_CLKGATE_CON(0) bit 9
 *     bank1 : PCLK_GPIO1  CLKGATE_CON(3)  bit 8
 *             DBCLK_GPIO1 CLKGATE_CON(3)  bit 9
 *     bank2 : PCLK_GPIO2  CLKGATE_CON(12) bit 14
 *             DBCLK_GPIO2 CLKGATE_CON(12) bit 15
 *     bank3 : PCLK_GPIO3  CLKGATE_CON(13) bit 0
 *             DBCLK_GPIO3 CLKGATE_CON(13) bit 1
 *     bank4 : PCLK_GPIO4  CLKGATE_CON(13) bit 2
 *             DBCLK_GPIO4 CLKGATE_CON(13) bit 3
 *
 *   The IOC-side clocks (PCLK_GPIO0_IOC / PCLK_GPIO1_IOC /
 *   PCLK_GPIO234_IOC) are all marked CLK_IS_CRITICAL in the kernel and
 *   are already ungated by BootROM/U-Boot, so they are not touched.
 *
 *   The DBCLK muxes (PMU_CLKSEL_CON(0) / CLKSEL_CON(35)) are left at
 *   their reset parents (xin24m / 32k); only the gates are opened.
 *
 * Input Parameters:
 *   bank - GPIO bank number, 0..4
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL for an out-of-range bank.
 *
 ****************************************************************************/

#define CLKGATE3_PCLK_GPIO1    (1u << 8)
#define CLKGATE3_DBCLK_GPIO1   (1u << 9)
#define CLKGATE12_PCLK_GPIO2   (1u << 14)
#define CLKGATE12_DBCLK_GPIO2  (1u << 15)
#define CLKGATE13_PCLK_GPIO3   (1u << 0)
#define CLKGATE13_DBCLK_GPIO3  (1u << 1)
#define CLKGATE13_PCLK_GPIO4   (1u << 2)
#define CLKGATE13_DBCLK_GPIO4  (1u << 3)

int rk3506_gpio_clock_enable(uint8_t bank)
{
  switch (bank)
    {
      case 0:
        putreg32((PMU_CLKGATE0_PCLK_GPIO0 | PMU_CLKGATE0_DBCLK_GPIO0)
                 << 16, RK3506_PMUCRU_CLKGATE_CON(0));
        break;

      case 1:
        putreg32((CLKGATE3_PCLK_GPIO1 | CLKGATE3_DBCLK_GPIO1) << 16,
                 RK3506_CRU_CLKGATE_CON(3));
        break;

      case 2:
        putreg32((CLKGATE12_PCLK_GPIO2 | CLKGATE12_DBCLK_GPIO2) << 16,
                 RK3506_CRU_CLKGATE_CON(12));
        break;

      case 3:
        putreg32((CLKGATE13_PCLK_GPIO3 | CLKGATE13_DBCLK_GPIO3) << 16,
                 RK3506_CRU_CLKGATE_CON(13));
        break;

      case 4:
        putreg32((CLKGATE13_PCLK_GPIO4 | CLKGATE13_DBCLK_GPIO4) << 16,
                 RK3506_CRU_CLKGATE_CON(13));
        break;

      default:
        return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3506_all_clocks_enable
 *
 * Description:
 *   Debug aid: ungate EVERY clock in both the main CRU (CLKGATE_CON0..22)
 *   and the PMU CRU (PMU_CLKGATE_CON0..2).  Rockchip gate registers use
 *   HIWORD write-enable + SET_TO_DISABLE data semantics, so 0xFFFF0000
 *   means "for every gate: enable (data bit 0)"; reserved bits ignore the
 *   write.  Counts come from clk-rk3506.c (highest used index = 22 main,
 *   2 PMU).  Dividers/muxes are left untouched; per-peripheral init still
 *   programs its own mux/div as needed.
 *
 *   DEBUG-ONLY escape hatch for bring-up (e.g. "is this peripheral dead
 *   because its clock is gated?").  It has no production caller -- each
 *   peripheral ungates its own clocks through the helpers above -- so it
 *   is GC'd by LTO in a normal build.  Do NOT call it from production
 *   paths; doing so masks real clock-ownership bugs.
 *
 ****************************************************************************/

void rk3506_all_clocks_enable(void)
{
  unsigned int i;

  for (i = 0; i <= 22; i++)
    {
      putreg32(0xFFFF0000u, RK3506_CRU_CLKGATE_CON(i));
    }

  for (i = 0; i <= 2; i++)
    {
      putreg32(0xFFFF0000u, RK3506_PMUCRU_CLKGATE_CON(i));
    }
}

/****************************************************************************
 * Name: rk3506_saradc_clock_init
 *
 * Description:
 *   Ungate the SARADC clocks and release its resets (clk-rk3506.c):
 *
 *     PCLK_SARADC : GATE, CLKGATE_CON(19), bit 0
 *     CLK_SARADC  : COMPOSITE, CLKSEL_CON(54) mux[5:4] / div[3:0],
 *                   gate CLKGATE_CON(19), bit 1
 *     SRST_P_SARADC / SRST_SARADC : SOFTRST_CON(19), bits 0/1
 *
 *   The converter clock mux stays on xin24m (24 MHz) with div = 15,
 *   i.e. 24 MHz / 16 = 1.5 MHz - the closest rate reachable with the
 *   4-bit divider (the kernel CCF makes the same choice for its
 *   1 MHz request).
 *
 ****************************************************************************/

#define CRU_CLKGATE19_PCLK_SARADC   (1u << 0)
#define CRU_CLKGATE19_CLK_SARADC    (1u << 1)
#define CRU_SOFTRST19_SRST_P_SARADC (1u << 0)
#define CRU_SOFTRST19_SRST_SARADC   (1u << 1)

#define SARADC_CLKSEL54_MUX_SHIFT   4u
#define SARADC_CLKSEL54_MUX_MASK    (0x3u << SARADC_CLKSEL54_MUX_SHIFT)
#define SARADC_CLKSEL54_DIV_MASK    (0xfu << 0)
#define SARADC_CLKSEL54_DIV_1P5MHZ  15u

void rk3506_saradc_clock_init(void)
{
  uint32_t clksel;

  /* Ungate PCLK_SARADC + CLK_SARADC */

  putreg32((CRU_CLKGATE19_PCLK_SARADC | CRU_CLKGATE19_CLK_SARADC) << 16,
           RK3506_CRU_CLKGATE_CON(19));

  /* CLK_SARADC: mux = xin24m (0), div = 15 -> 24 MHz / 16 = 1.5 MHz */

  clksel = getreg32(RK3506_CRU_CLKSEL_CON(54));
  clksel &= ~(SARADC_CLKSEL54_MUX_MASK | SARADC_CLKSEL54_DIV_MASK);
  clksel |= SARADC_CLKSEL54_DIV_1P5MHZ;
  putreg32((0x3fu << 16) | clksel, RK3506_CRU_CLKSEL_CON(54));

  /* Release SRST_P_SARADC + SRST_SARADC */

  putreg32((CRU_SOFTRST19_SRST_P_SARADC | CRU_SOFTRST19_SRST_SARADC)
           << 16, RK3506_CRU_SOFTRST_CON(19));
}

/****************************************************************************
 * Name: rk3506_can_clock_init
 *
 * Description:
 *   Bring up one CAN controller's clocks and release its resets
 *   (clk-rk3506.c lines 493-502 + SRST ids 212-215):
 *
 *     HCLK_CAN0/1 : GATE, CLKGATE_CON(13), bits 4/6
 *     CLK_CAN0/1  : COMPOSITE, CLKSEL_CON(35/36) mux[13:11]/[7:5],
 *                   div[10:6]/[4:0], gate CLKGATE_CON(13) bits 5/7
 *     SRST_H_CANx / SRST_CANx : SOFTRST_CON(13), bits 4/5 or 6/7
 *
 *   The composite mux parent table is
 *     { xin24m, gpll, clk_v0pll_gate, clk_v1pll_gate, ... }
 *   mux = gpll (1), div = 5 -> 1200 MHz / 6 = 200 MHz, the rate the
 *   HAL CANFD bit-timing tables are calibrated for.
 *
 * Input Parameters:
 *   iface - CAN interface number (0 or 1).
 *
 ****************************************************************************/

#define CRU_CLKGATE13_HCLK_CAN0 (1u << 4)
#define CRU_CLKGATE13_CLK_CAN0  (1u << 5)
#define CRU_CLKGATE13_HCLK_CAN1 (1u << 6)
#define CRU_CLKGATE13_CLK_CAN1  (1u << 7)
#define CRU_SOFTRST13_H_CAN0    (1u << 4)
#define CRU_SOFTRST13_CAN0      (1u << 5)
#define CRU_SOFTRST13_H_CAN1    (1u << 6)
#define CRU_SOFTRST13_CAN1      (1u << 7)

#define CAN_CLKSEL_MUX_SHIFT0   11u   /* CLK_CAN0 */
#define CAN_CLKSEL_MUX_SHIFT1   5u    /* CLK_CAN1 */
#define CAN_CLKSEL_DIV_SHIFT0   6u
#define CAN_CLKSEL_DIV_SHIFT1   0u
#define CAN_CLKSEL_DIV_200MHZ   5u
#define CAN_CLKSEL_MUX_GPLL     1u

void rk3506_can_clock_init(unsigned int iface)
{
  uint32_t gatebits;
  uint32_t rstbits;
  uint32_t clksel;
  uint32_t muxshift;
  uint32_t divshift;
  uint32_t selcon;
  uint32_t wemask;

  if (iface == 0)
    {
      gatebits = CRU_CLKGATE13_HCLK_CAN0 | CRU_CLKGATE13_CLK_CAN0;
      rstbits  = CRU_SOFTRST13_H_CAN0 | CRU_SOFTRST13_CAN0;
      muxshift = CAN_CLKSEL_MUX_SHIFT0;
      divshift = CAN_CLKSEL_DIV_SHIFT0;
      selcon   = 35u;

      /* CAN0 fields occupy CLKSEL_CON35 data bits [13:6]
       * (mux [13:11], div [10:6]); the HIWORD write-enable mask
       * must cover exactly those bits (kernel clk-rk3506.c:495-499).
       * The previous fixed 0x7f mask only enabled the low seven
       * bits and silently dropped the CAN0 mux/divisor writes.
       */

      wemask = (0x7u << CAN_CLKSEL_MUX_SHIFT0) |
               (0x1fu << CAN_CLKSEL_DIV_SHIFT0);   /* 0x3fc0 */
    }
  else
    {
      gatebits = CRU_CLKGATE13_HCLK_CAN1 | CRU_CLKGATE13_CLK_CAN1;
      rstbits  = CRU_SOFTRST13_H_CAN1 | CRU_SOFTRST13_CAN1;
      muxshift = CAN_CLKSEL_MUX_SHIFT1;
      divshift = CAN_CLKSEL_DIV_SHIFT1;
      selcon   = 36u;

      /* CAN1 fields occupy CLKSEL_CON36 data bits [7:0]
       * (mux [7:5], div [4:0]) (kernel clk-rk3506.c:500-502).
       */

      wemask = (0x7u << CAN_CLKSEL_MUX_SHIFT1) |
               (0x1fu << CAN_CLKSEL_DIV_SHIFT1);   /* 0x00ff */
    }

  /* Ungate HCLK + CLK */

  putreg32(gatebits << 16, RK3506_CRU_CLKGATE_CON(13));

  /* CLK_CANx: mux = gpll, div = 5 -> 200 MHz */

  clksel = getreg32(RK3506_CRU_CLKSEL_CON(selcon));
  clksel &= ~((0x7u << muxshift) | (0x1fu << divshift));
  clksel |= (CAN_CLKSEL_MUX_GPLL << muxshift) |
            (CAN_CLKSEL_DIV_200MHZ << divshift);
  putreg32((wemask << 16) | clksel, RK3506_CRU_CLKSEL_CON(selcon));

  /* Release both resets */

  putreg32(rstbits << 16, RK3506_CRU_SOFTRST_CON(13));
}

/****************************************************************************
 * Name: rk3506_wdt_clock_init
 *
 * Description:
 *   Bring up one DW watchdog's clocks (clk-rk3506.c lines 349-356):
 *
 *     PCLK_WDT0/1 : GATE, CLKGATE_CON(6), bits 9/11 (pclk_bus_root)
 *     TCLK_WDT0/1 : GATE, CLKGATE_CON(6), bits 10/12 (xin24m, 24 MHz,
 *                   the watchdog counter clock)
 *
 *   The DW watchdog on RK3506 has no dedicated reset line in the
 *   cru dts node (resets are internal to the controller enable).
 *
 * Input Parameters:
 *   iface - Watchdog interface number (0 = WDT0, 1 = WDT1).
 *
 ****************************************************************************/

#define CRU_CLKGATE6_PCLK_WDT0  (1u << 9)
#define CRU_CLKGATE6_TCLK_WDT0  (1u << 10)
#define CRU_CLKGATE6_PCLK_WDT1  (1u << 11)
#define CRU_CLKGATE6_TCLK_WDT1  (1u << 12)

void rk3506_wdt_clock_init(unsigned int iface)
{
  uint32_t bits = (iface == 0) ?
    (CRU_CLKGATE6_PCLK_WDT0 | CRU_CLKGATE6_TCLK_WDT0) :
    (CRU_CLKGATE6_PCLK_WDT1 | CRU_CLKGATE6_TCLK_WDT1);

  putreg32(bits << 16, RK3506_CRU_CLKGATE_CON(6));
}
