/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_st7701s.h
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

#ifndef __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_ST7701S_H
#define __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_ST7701S_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_st7701s_initialize
 *
 * Description:
 *   Initialize the ST7701S LCD panel via 3-wire SPI (bit-bang).
 *   Sends the initialization sequence to configure the panel for
 *   480x854 RGB565 operation, then the VOP drives the RGB interface.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_LCD_ST7701S
int hd_rk3506_st7701s_initialize(void);
#else
#  define hd_rk3506_st7701s_initialize() (0)
#endif

#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_ST7701S_H */
