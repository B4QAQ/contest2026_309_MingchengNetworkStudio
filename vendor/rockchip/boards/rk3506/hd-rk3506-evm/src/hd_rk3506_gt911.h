/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_gt911.h
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

#ifndef __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_GT911_H
#define __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_GT911_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_gt911_initialize
 *
 * Description:
 *   Initialize the GT911 touchscreen controller on I2C0.
 *
 *   Performs hardware reset, verifies GT911 presence via product ID
 *   register, registers /dev/input0, and starts 10ms polling.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_INPUT_GT911
int hd_rk3506_gt911_initialize(void);
#else
#  define hd_rk3506_gt911_initialize() (0)
#endif

#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_GT911_H */
