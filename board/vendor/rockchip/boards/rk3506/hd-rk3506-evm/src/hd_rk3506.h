/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506.h
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

#ifndef __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_H
#define __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Functions Definitions
 ****************************************************************************/

int hd_rk3506_bringup(void);

#ifdef CONFIG_GRAPHICS_LVGL
int hd_rk3506_lv_port_disp_init(void);

/* ESC button handlers (see hd_rk3506_lv_port_indev.c for why the back
 * key is dispatched here instead of through the LVGL group focus).
 */

typedef CODE void (*hd_rk3506_back_handler_t)(FAR void *arg);

int hd_rk3506_back_register(hd_rk3506_back_handler_t cb, FAR void *arg);
void hd_rk3506_back_unregister(hd_rk3506_back_handler_t cb);
#endif

#ifdef CONFIG_IOEXPANDER
int hd_rk3506_gpio_initialize(void);
#endif

#ifdef CONFIG_HD_RK3506_LED_HEARTBEAT
int hd_rk3506_led_heartbeat_start(void);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3506_HD_RK3506_EVM_SRC_HD_RK3506_H */
