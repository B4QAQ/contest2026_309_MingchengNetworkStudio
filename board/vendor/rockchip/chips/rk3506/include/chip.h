/****************************************************************************
 * vendor/rockchip/chips/rk3506/include/chip.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_INCLUDE_CHIP_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_INCLUDE_CHIP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3506G2 SoC Characteristics:
 *   - Cortex-A7 x3, ARMv7-A, 1.5GHz
 *   - 128MB internal DDR3
 *   - GICv2 interrupt controller (DIST: 0xFF581000, CPU: 0xFF582000)
 *   - DesignWare 8250 compatible UARTs
 *   - DWC2 USB OTG controllers
 */

/* UART Clock: SCLK_UART0 = 1.8432 MHz from CRU */

#define RK3506_UART_CLK       1843200

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_INCLUDE_CHIP_H */
