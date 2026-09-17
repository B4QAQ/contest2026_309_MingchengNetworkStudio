/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_timerisr.c
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

/* System tick using the ARM generic timer, in the same simple, direct
 * style as the working Allwinner r528 Cortex-A7 port
 * (vendor/allwinnertech/chips/r528/r528_timerisr.c): a one-shot physical
 * timer that reloads every tick and calls nxsched_process_timer(). No
 * oneshot/alarm framework involved.
 *
 * RK3506 is entered non-secure (U-Boot hands the kernel to OP-TEE which
 * ERETs to NS PL1), so the PL1 physical timer accessed by CP15 c14,c2 is
 * the NON-SECURE physical timer, which signals GIC PPI interrupt 30
 * (GIC_IRQ_PTM). (A secure boot like r528 uses PPI 29 instead.)
 */

#include <nuttx/config.h>
#include <stdint.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include "chip.h"

/* Non-secure physical timer: GIC PPI interrupt 30 */

#define RK3506_TIMER_IRQ     30

static uint32_t g_timer_reload;

static inline uint32_t arm_cntfrq(void)
{
  uint32_t v;
  __asm__ __volatile__("mrc p15, 0, %0, c14, c0, 0" : "=r"(v));
  return v;
}

static inline void arm_cntp_tval(uint32_t v)
{
  __asm__ __volatile__("mcr p15, 0, %0, c14, c2, 0" : : "r"(v));
}

static inline void arm_cntp_ctl(uint32_t v)
{
  __asm__ __volatile__("mcr p15, 0, %0, c14, c2, 1" : : "r"(v));
  __asm__ __volatile__("isb");
}

static int rk3506_tick(int irq, void *context, void *arg)
{
  /* Re-arm the one-shot physical timer for the next tick (the TVAL
   * register counts down), then drive the scheduler.
   */

  arm_cntp_ctl(0);                  /* disable timer */
  arm_cntp_tval(g_timer_reload);    /* reload */
  arm_cntp_ctl(1);                  /* enable timer (unmasked) */

  nxsched_process_timer();
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void up_timer_initialize(void)
{
  uint32_t freq = arm_cntfrq();

  if (freq == 0)
    {
      freq = 24000000;              /* RK3506 timer clock is 24 MHz */
    }

  g_timer_reload = freq / CONFIG_USEC_PER_TICK;   /* timer counts per tick */

  irq_attach(RK3506_TIMER_IRQ, rk3506_tick, NULL);

  arm_cntp_ctl(0);                  /* disable while programming */
  arm_cntp_tval(g_timer_reload);
  arm_cntp_ctl(1);                  /* enable timer (unmasked) */

  up_enable_irq(RK3506_TIMER_IRQ);
}
