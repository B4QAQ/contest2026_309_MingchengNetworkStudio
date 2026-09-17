/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_usbhost.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_USBHOST_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_USBHOST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/usb/usbhost.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of host channels */

#define RK3506_DWC2_NHOST_CHANNELS     12

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3506_usbhost_initialize
 *
 * Description:
 *   Initialize USB host controller hardware for the DWC2 OTG controller
 *   on the RK3506.
 *
 * Input Parameters:
 *   controller - 0 for OTG0, 1 for OTG1
 *
 * Returned Value:
 *   An instance of the USB host connection interface, or NULL on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3506_USBHOST
struct usbhost_connection_s *rk3506_usbhost_initialize(int controller);
#endif

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_USBHOST_H */
