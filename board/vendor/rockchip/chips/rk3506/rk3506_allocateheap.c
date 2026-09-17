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
#include <debug.h>

#include <nuttx/mm/mm.h>
#include <nuttx/arch.h>

#include "arm_internal.h"
#include "chip.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_allocateheap
 *
 * Description:
 *   This function is called by arm_addrenv_init() to allocate the heap.
 *   For the RK3506 in flat build mode, the heap is automatically allocated
 *   by the common ARM code from the end of BSS to the top of usable RAM.
 *
 ****************************************************************************/

void arm_allocateheap(void)
{
  /* For flat build, the common ARM code handles heap allocation.
   * No additional regions need to be added.
   *
   * If multiple memory regions are available, call umm_addregion()
   * to add additional heap regions.
   */
}
