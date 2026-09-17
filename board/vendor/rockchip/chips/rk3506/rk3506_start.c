/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_start.c
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

#include <stdint.h>

#include "arm_internal.h"
#include "chip.h"
#include "gic.h"
#include "mmu.h"
#include "rk3506_start.h"

#ifdef CONFIG_SCHED_INSTRUMENTATION
#  include <sched/sched.h>
#  include <nuttx/sched_note.h>
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_setupmappings
 *
 * Description:
 *   Setup the MMU page table mappings for RK3506.
 *   This configures the memory regions for device I/O and normal memory.
 *
 ****************************************************************************/

/*
 * Physical memory / I/O layout on RK3506G2 (HD-RK3506-EVM):
 *   DRAM: 0x00000000 - 0x07ffffff (128 MiB; U-Boot reports bank end 0x08000000)
 *   SoC peripherals: 0xff000000 - 0xffffffff
 *     UART 0xff0a0000, CRU ~0xff040000, GIC 0xff580000, VOP/USB/... 0xff7xxxxx
 *
 * arm_head.S builds a temporary L1 table mapping only the .text region
 * (and the RAM region because CONFIG_BOOT_RUNFROMFLASH is set).  It does
 * NOT map the peripheral block, so the very first device register access
 * once the MMU is on (UART/GIC/clock) would data abort.  This fills the
 * L1 table with full RAM (cached) + peripheral (device, XN) sections.
 */

static const struct section_mapping_s g_rk3506_mappings[] =
{
  /* Identity-map DRAM 0x00000000..0x03c00000 as cached normal memory
   * (60 x 1MB sections; the NuttX RAM 0x02080000..0x03c00000 lives
   * here).
   */

  { 0x00000000u, 0x00000000u, MMU_MEMFLAGS, 0x03c },  /* 60 x 1MB sections */

  /* The rpmsg shared window 0x03c00000..0x03e00000 (vring0/vring1 +
   * buffer carveout, shared with the M0) must be UNCACHED device
   * memory: the Cortex-M0 has no cache and no coherency with the A7,
   * so with a cached A7 view the vring descriptors and the avail/used
   * indexes stick in the A7 dcache while the M0 reads stale data from
   * DDR - the rpmsg data plane never synchronizes (v8 root cause of
   * the rpmsgtest timeout).  open-amp touches the vring metadata with
   * plain stores/loads, so the mapping itself must guarantee the
   * coherency.
   */

  { 0x03c00000u, 0x03c00000u, MMU_IOFLAGS, 0x002 },   /* 2 x 1MB sections  */

  /* Rest of the DRAM window, cached again (0x03e00000..0x08000000). */

  { 0x03e00000u, 0x03e00000u, MMU_MEMFLAGS, 0x042 },  /* 66 x 1MB sections */

  /* Identity-map the peripheral register block (0xff000000..0xffffffff). */

  { 0xff000000u, 0xff000000u, MMU_IOFLAGS, 0x010 },   /* 16 x 1MB sections  */
};

static void rk3506_setupmappings(void)
{
  mmu_l1_map_regions(g_rk3506_mappings,
                     sizeof(g_rk3506_mappings) /
                     sizeof(struct section_mapping_s));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_boot
 *
 * Description:
 *   Complete boot operations started in arm_head.S
 *
 ****************************************************************************/

void arm_boot(void)
{
  /* Setup page table mappings (this also maps the peripheral region) */

  rk3506_setupmappings();

  /* Configure FPU */

  arm_fpuconfig();

#ifdef USE_EARLYSERIALINIT
  /* Perform early serial initialization if we are going to use the serial
   * driver.
   */

  arm_earlyserialinit();
#endif
}

#if defined(CONFIG_NET) && !defined(CONFIG_NETDEV_LATEINIT)
void arm_netinitialize(void)
{
  /* Network device initialization - to be implemented */
}
#endif
