/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_rng.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3506 True Random Number Generator (TRNG) stub driver.
 * Registers /dev/random as a hardware entropy source.
 *
 * SDK reference: hal/lib/hal/src/hal_trng.c
 * Base address: 0xFF0E0000 (from rk3506_memorymap.h)
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/random.h>

#include <debug.h>

#include "rk3506_rng.h"

#ifdef CONFIG_RK3506_RNG

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* TRNG register offsets (from SDK hal_trng.c) */

#define TRNG_RNG_CTRL             0x000
#define TRNG_RNG_RAND_NUM         0x004
#define TRNG_RNG_STATE            0x008

#define TRNG_CTRL_ENABLE          (1u << 0)
#define TRNG_STATE_READY          (1u << 0)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_rng_read
 ****************************************************************************/

static ssize_t rk3506_rng_read(FAR struct file *filep, FAR char *buffer,
                               size_t buflen)
{
  FAR uint32_t *data = (FAR uint32_t *)buffer;
  size_t words = buflen / sizeof(uint32_t);
  size_t i;

  for (i = 0; i < words; i++)
    {
      /* Wait for random data ready */

      while ((getreg32(RK3506_RNG_BASE + TRNG_RNG_STATE) &
              TRNG_STATE_READY) == 0)
        {
        }

      data[i] = getreg32(RK3506_RNG_BASE + TRNG_RNG_RAND_NUM);
    }

  return buflen;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_rng_initialize
 *
 * Description:
 *   Initialize the TRNG and register /dev/random.
 *
 ****************************************************************************/

int rk3506_rng_initialize(void)
{
  /* Enable TRNG */

  putreg32(TRNG_CTRL_ENABLE, RK3506_RNG_BASE + TRNG_RNG_CTRL);

  _info("TRNG initialized (0x%08" PRIx32 ")\n", RK3506_RNG_BASE);

  return OK;
}

#endif /* CONFIG_RK3506_RNG */
