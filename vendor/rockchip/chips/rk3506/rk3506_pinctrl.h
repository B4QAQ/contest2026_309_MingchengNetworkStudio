/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_pinctrl.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unified pin-control (IOMUX / RM_IO / pull / drive / schmitt) for the
 * Rockchip RK3506G2.
 *
 * This is the single, authoritative entry point for ALL pin
 * configuration on the RK3506.  It replaces the scattered, hard-coded
 * register pokes that used to live in rk3506_iomux.c, rk3506_pwm.c,
 * hd_rk3506_nv3007.c, rk3506_gmac0.c, ...
 *
 * Every register offset, bit field and write sequence below is taken
 * directly from the vendor SDK so that the values are exact:
 *
 *   - U-Boot  : drivers/pinctrl/rockchip/pinctrl-rk3506.c
 *               (rk3506_set_mux / rockchip_set_rmio / rk3506_calc_drv /
 *                rk3506_calc_pull / rk3506_calc_schmitt)
 *   - Kernel  : drivers/pinctrl/pinctrl-rockchip.c (RK3506 section)
 *   - CMSIS   : hal/lib/CMSIS/Device/RK3506/Include/rk3506.h
 *               (struct GPIO0_IOC_REG / GPIO1_IOC_REG / RM_IO_REG)
 *
 * Register model (all writes are Rockchip HIWORD_UPDATE: the high
 * half-word is a per-field write-enable mask, the low half-word the
 * data):
 *
 *   IOMUX : 4-bit function select per pin, 4 pins per 32-bit register.
 *   RM_IO : 32-channel routing matrix at GRF_PMU (0xFF910000).  A pin
 *           whose "mux" number exceeds the 4-bit field (>15) is an
 *           RM_IO pin: the routing function is (mux - 15) written into
 *           the channel register (FUNC_SEL[6:0]) and the local IOMUX
 *           nibble is then forced to 7 (the matrix path).
 *   PULL  : 2-bit per pin (0=disable 1=up 2=down 3=bus-hold for the
 *           default pull type; GPIO0_D/GPIO4 use the 1V8 type where
 *           1=down 3=up).
 *   DRIVE : 8-bit thermometer code per pin, 2 pins per register; the
 *           value is (1<<(level+1))-1 for level 0..7 (GPIO0_D/GPIO4 use
 *           a plain 2-bit level).
 *   SMT   : 1-bit schmitt trigger enable per pin.
 *
 * Per-bank IOC base addresses (these differ per bank -- getting them
 * wrong is the historical M2b.1/M4.1 class of bug):
 *
 *   bank0 -> IOC_PMU  0xFF950000
 *   bank1 -> IOC1     0xFF660000   (NOT the shared IOC_GRF!)
 *   bank2 -> IOC_GRF  0xFF4D8000
 *   bank3 -> IOC_GRF  0xFF4D8000
 *   bank4 -> IOC_GRF  0xFF4D8000   (mux is a no-op; pins are GPIO-only)
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_PINCTRL_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_PINCTRL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Pull configuration (matches the kernel PIN_CONFIG_BIAS_* semantics) */

#define RK3506_PULL_DISABLE    0
#define RK3506_PULL_UP         1
#define RK3506_PULL_DOWN       2
#define RK3506_PULL_BUS_HOLD   3

/* RM_IO matrix function-select values for a pin are passed to
 * rk3506_pinctrl_set_mux() as the SDK's "mux" number (>15).  The helper
 * below converts between the DTS mux number and the matrix FUNC_SEL.
 */

#define RK3506_RMIO_MUX_TO_FUNCSEL(mux)   ((mux) - 15)
#define RK3506_RMIO_FUNCSEL_TO_MUX(fn)    ((fn) + 15)

/* The local IOMUX value that selects the RM_IO matrix path */

#define RK3506_RMIO_IOMUX_FUNC   7u

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3506_pinctrl_set_mux
 *
 * Description:
 *   Select the function of one pin.
 *
 *   For a directly-muxed peripheral (func 0..15) this writes the pin's
 *   4-bit IOMUX nibble.  For an RM_IO-matrix pin (func > 15, i.e. the
 *   DTS mux number) it programs the RM_IO routing channel with
 *   (func - 15) and then forces the local IOMUX nibble to 7, exactly as
 *   the SDK's rockchip_set_rmio() + rk3506_set_mux() do.
 *
 * Input Parameters:
 *   bank - GPIO bank number, 0..4
 *   pin  - Pin number within the bank, 0..31
 *   func - Function select: 0..15 for a direct mux, >15 for an RM_IO
 *          routing (the SDK/DTS mux number).
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL on an out-of-range bank/pin/func or on a
 *   bank/pin combination that has no RM_IO channel.
 *
 ****************************************************************************/

int rk3506_pinctrl_set_mux(uint8_t bank, uint8_t pin, uint8_t func);

/****************************************************************************
 * Name: rk3506_pinctrl_get_mux
 *
 * Description:
 *   Read back the current 4-bit IOMUX function select of a pin.
 *   Used to verify that set_mux() actually landed on the hardware.
 *
 * Input Parameters:
 *   bank - GPIO bank number, 0..4
 *   pin  - Pin number within the bank, 0..31
 *
 * Returned Value:
 *   The 4-bit function value (0..15), or -EINVAL on bad bank/pin.
 *
 ****************************************************************************/

int rk3506_pinctrl_get_mux(uint8_t bank, uint8_t pin);

/****************************************************************************
 * Name: rk3506_pinctrl_set_pull
 *
 * Description:
 *   Set the pull-up/pull-down of one pin (2-bit field, HIWORD_UPDATE).
 *
 * Input Parameters:
 *   bank - GPIO bank number, 0..4
 *   pin  - Pin number within the bank, 0..31
 *   pull - One of RK3506_PULL_*.
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL on an out-of-range bank/pin.
 *
 ****************************************************************************/

int rk3506_pinctrl_set_pull(uint8_t bank, uint8_t pin, uint8_t pull);

/****************************************************************************
 * Name: rk3506_pinctrl_set_drive
 *
 * Description:
 *   Set the drive strength of one pin.
 *
 *   level is the drive-strength index (0..7) as used by the SDK; the
 *   register is programmed with the thermometer code (1<<(level+1))-1,
 *   or with the raw 2-bit level for the special GPIO0_D/GPIO4 pins.
 *
 * Input Parameters:
 *   bank - GPIO bank number, 0..4
 *   pin  - Pin number within the bank, 0..31
 *   level- Drive strength index, 0..7.
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL on an out-of-range bank/pin.
 *
 ****************************************************************************/

int rk3506_pinctrl_set_drive(uint8_t bank, uint8_t pin, uint8_t level);

/****************************************************************************
 * Name: rk3506_pinctrl_set_schmitt
 *
 * Description:
 *   Enable/disable the schmitt-trigger input of one pin (1-bit field).
 *
 * Input Parameters:
 *   bank   - GPIO bank number, 0..4
 *   pin    - Pin number within the bank, 0..31
 *   enable - true to enable the schmitt trigger.
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL on an out-of-range bank/pin.
 *
 ****************************************************************************/

int rk3506_pinctrl_set_schmitt(uint8_t bank, uint8_t pin, bool enable);

/****************************************************************************
 * Name: rk3506_pinctrl_rmio_func
 *
 * Description:
 *   Query whether a (bank, pin) carries an RM_IO channel and, if so,
 *   return its channel register offset.  Exposed so board code and
 *   drivers can validate routing tables at build time.
 *
 * Input Parameters:
 *   bank - GPIO bank number, 0..4
 *   pin  - Pin number within the bank, 0..31
 *
 * Returned Value:
 *   The RM_IO channel register offset from GRF_PMU (>= 0) if the pin is
 *   RM_IO-capable; -EINVAL otherwise.
 *
 ****************************************************************************/

int rk3506_pinctrl_rmio_offset(uint8_t bank, uint8_t pin);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_PINCTRL_H */
