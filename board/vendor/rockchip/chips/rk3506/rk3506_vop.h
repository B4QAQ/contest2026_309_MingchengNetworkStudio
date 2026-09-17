/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_vop.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_VOP_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_VOP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* VOP Register Offsets (RK3366_LIT / VOP 2.14) */

#define VOP_REG_CFG_DONE         0x00000
#define VOP_SYS_CTRL0            0x00010
#define VOP_SYS_CTRL1            0x00014
#define VOP_SYS_CTRL2            0x00018
#define VOP_DSP_BG               0x00008
#define VOP_DSP_CTRL0            0x00020
#define VOP_DSP_CTRL1            0x00024
#define VOP_DSP_CTRL2            0x00028

/* Display Timing Registers */

#define VOP_DSP_HTOTAL_HS_END    0x00100
#define VOP_DSP_HACT_ST_END      0x00104
#define VOP_DSP_VTOTAL_VS_END    0x00108
#define VOP_DSP_VACT_ST_END      0x0010C

/* Window 1 (Primary) Registers */

#define VOP_WIN1_CTRL0           0x00090
#define VOP_WIN1_CTRL1           0x00094
#define VOP_WIN1_VIR             0x00098
#define VOP_WIN1_MST             0x000A0
#define VOP_WIN1_DSP_INFO        0x000A4
#define VOP_WIN1_DSP_ST          0x000A8
#define VOP_WIN1_ALPHA_CTRL      0x000BC

/* DSP_CTRL0 bits */

#define VOP_DSP_CTRL0_RGB_EN     (1 << 0)

/* REG_CFG_DONE trigger */

#define VOP_REG_CFG_TRIGGER      0x01

/* WIN1_CTRL0 bits (verified against Linux rk3506_lit_win1_data) */

#define VOP_WIN1_CTRL0_EN        (1 << 0)
#define VOP_WIN1_FMT_SHIFT       4
#define VOP_WIN1_FMT_MASK        (0x7 << VOP_WIN1_FMT_SHIFT)

/* VOP data formats (WIN1_CTRL0[6:4]) */

#define VOP_FMT_ARGB8888         0
#define VOP_FMT_RGB888           1
#define VOP_FMT_RGB565           2
#define VOP_FMT_YCbCr420         4

/* GRF register for RGB interface (verified against Linux rk3506_grf_ctrl) */

#define GRF_SOC_CON2             0x00008  /* Offset from GRF base (RK3506_GRF_SOC_CON2) */

/* RK3506_GRF_SOC_CON2 layout (verified against Linux kernel 6.1):
 *   bit 0: grf_dclk_inv (single bit)
 *   bit 1: data_bypass (0x3 = bypass both, 0x0 = no bypass)
 *   bits [4..10]: DLL select (4-bit value 0..15, scaled by some factor)
 *   bits [11..15]: reserved
 *
 * HIWORD_UPDATE pattern: writes upper 16 bits as data, lower 16 bits as mask.
 *   data | (mask << 16)  ->  reg = (reg & ~mask) | (data & mask)
 */

#define GRF_SOC_CON2_DLL_SEL_RGB    0x10   /* RGB interface DLL value */
#define GRF_SOC_CON2_DLL_SEL_BT1120 0x20   /* BT1120/BT656 DLL value */
#define GRF_SOC_CON2_DATA_BYPASS     0x3    /* 0x3 = bypass data path */

#define GRF_SOC_CON2_DLL_SEL_MASK    (0x7f << 4)
#define GRF_SOC_CON2_DLL_SEL_SHIFT   4
#define GRF_SOC_CON2_BYPASS_MASK     (0x1 << 1)
#define GRF_SOC_CON2_BYPASS_SHIFT    1

/* HIWORD_UPDATE helper macro - assembles data | (mask << 16) */

#define GRF_HIWORD_UPDATE(val, mask, shift) \
  (((val) << (shift)) | ((mask) << ((shift) + 16)))

/* Display timing parameters for ST7701S 480x854 @ 30MHz */

#define RK3506_VOP_XRES          480
#define RK3506_VOP_YRES          854
#define RK3506_VOP_BPP           16  /* RGB565 */
#define RK3506_VOP_STRIDE        (RK3506_VOP_XRES * (RK3506_VOP_BPP / 8))
#define RK3506_VOP_FBSIZE        (RK3506_VOP_STRIDE * RK3506_VOP_YRES)

/* Horizontal timing */

#define RK3506_VOP_HACT          480
#define RK3506_VOP_HFP           10
#define RK3506_VOP_HBP           10
#define RK3506_VOP_HSYNC         4
#define RK3506_VOP_HTOTAL        (RK3506_VOP_HACT + RK3506_VOP_HFP + \
                                  RK3506_VOP_HBP + RK3506_VOP_HSYNC)

/* Vertical timing */

#define RK3506_VOP_VACT          854
#define RK3506_VOP_VFP           10
#define RK3506_VOP_VBP           10
#define RK3506_VOP_VSYNC         4
#define RK3506_VOP_VTOTAL        (RK3506_VOP_VACT + RK3506_VOP_VFP + \
                                  RK3506_VOP_VBP + RK3506_VOP_VSYNC)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_vop_register
 *
 * Description:
 *   Initialize the RK3506 VOP and register /dev/fb0.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int rk3506_vop_register(void);

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3506_RK3506_VOP_H */
