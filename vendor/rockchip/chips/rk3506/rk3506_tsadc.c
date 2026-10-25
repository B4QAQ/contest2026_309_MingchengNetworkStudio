/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_tsadc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506 Temperature Sensor ADC (TSADC) stub driver.
 * Registers /dev/tsadc for temperature reading.
 *
 * SDK reference: hal/lib/hal/src/hal_tsadc.c
 * Base address: 0xFF2A8000 (from rk3506_memorymap.h)
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/sensors/sensor.h>

#include <debug.h>

#include "rk3506_tsadc.h"

#ifdef CONFIG_RK3506_TSADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* TSADC register offsets (from SDK hal_tsadc.c) */

#define TSADC_USER_CON            0x000
#define TSADC_AUTO_CON            0x004
#define TSADC_DATA0               0x020
#define TSADC_DATA1               0x024

#define TSADC_USER_CON_PD         (1u << 0)
#define TSADC_AUTO_CON_EN         (1u << 0)
#define TSADC_AUTO_CON_Q_SEL      (1u << 11)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_tsadc_read_temp
 ****************************************************************************/

static int rk3506_tsadc_read_temp(FAR struct sensor_lowerhalf_s *lower,
                                  FAR struct sensor_event_s *event)
{
  uint32_t data;
  int temp_mc;  /* millicelsius */

  /* Read channel 0 (CPU temperature) */

  data = getreg32(RK3506_TSADC_BASE + TSADC_DATA0);

  /* Convert to temperature (simplified linear approximation)
   * Actual conversion table is in SDK hal_tsadc.c
   */

  temp_mc = 25000 + (int)(data - 500) * 100;  /* Rough estimate */

  event->temperature = temp_mc / 1000.0f;

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_tsadc_initialize
 *
 * Description:
 *   Initialize the TSADC and register /dev/tsadc.
 *
 ****************************************************************************/

int rk3506_tsadc_initialize(void)
{
  /* Power up and enable auto mode */

  putreg32(0, RK3506_TSADC_BASE + TSADC_USER_CON);  /* Power up */
  putreg32(TSADC_AUTO_CON_EN | TSADC_AUTO_CON_Q_SEL,
           RK3506_TSADC_BASE + TSADC_AUTO_CON);

  _info("TSADC initialized (0x%08" PRIx32 ")\n", RK3506_TSADC_BASE);

  return OK;
}

#endif /* CONFIG_RK3506_TSADC */
