/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_allocateheap.c
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

#include <sys/types.h>
#include <stdint.h>

#include <nuttx/mm/mm.h>

#include "arm_internal.h"
#include "chip.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Defaults for the non-contiguous second heap region (see RK3506_HEAP2_*
 * Kconfig).  The window starts right after the four 1 MiB AMP carveouts
 * (0x03a00000..0x03e00000) and leaves the top 4 MiB of the 128 MiB DDR
 * unused as a U-Boot relocation margin.
 */

#ifndef CONFIG_RK3506_HEAP2_BASE
#  define RK3506_HEAP2_BASE   0x03e00000u
#else
#  define RK3506_HEAP2_BASE   CONFIG_RK3506_HEAP2_BASE
#endif

#ifndef CONFIG_RK3506_HEAP2_SIZE
#  define RK3506_HEAP2_SIZE   0x03e00000u   /* 62 MiB, ends at 0x07c00000 */
#else
#  define RK3506_HEAP2_SIZE   CONFIG_RK3506_HEAP2_SIZE
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_addregion
 *
 * Description:
 *   Add the upper DRAM window as a second heap region.  The primary heap
 *   (g_idle_topstack..CONFIG_RAM_END, ending at 0x03a00000) is created by
 *   the common ARM flat-build code; this region covers
 *   RK3506_HEAP2_BASE..RK3506_HEAP2_BASE + RK3506_HEAP2_SIZE.  The whole
 *   window is already identity-mapped cached by rk3506_start.c.
 *
 ****************************************************************************/

#if CONFIG_MM_REGIONS > 1
void arm_addregion(void)
{
  umm_addregion((FAR void *)RK3506_HEAP2_BASE, RK3506_HEAP2_SIZE);
}
#endif
