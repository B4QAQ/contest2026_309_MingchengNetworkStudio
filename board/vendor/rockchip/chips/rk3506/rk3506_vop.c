/****************************************************************************
 * vendor/rockchip/chips/rk3506/rk3506_vop.c
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

#include <debug.h>
#include <errno.h>
#include <string.h>

#include <nuttx/config.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>
#include <nuttx/video/fb.h>

#include "hardware/rk3506_memorymap.h"
#include "rk3506_vop.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef putreg32
#  define putreg32(v, x) (*(FAR volatile uint32_t *)(x) = (v))
#endif

#ifndef getreg32
#  define getreg32(x) (*(FAR volatile uint32_t *)(x))
#endif

/* Helper to build DSP_HTOTAL_HS_END / DSP_HACT_ST_END values */

#define VOP_HTOTAL_HS_END(hsync, htotal) \
  ((uint32_t)(((hsync) & 0xffff) | (((htotal) & 0xffff) << 16)))

#define VOP_HACT_ST_END(hbp, hact) \
  ((uint32_t)(((hbp) & 0xffff) | ((((hbp) + (hact)) & 0xffff) << 16)))

#define VOP_VTOTAL_VS_END(vsync, vtotal) \
  ((uint32_t)(((vsync) & 0xffff) | (((vtotal) & 0xffff) << 16)))

#define VOP_VACT_ST_END(vbp, vact) \
  ((uint32_t)(((vbp) & 0xffff) | ((((vbp) + (vact)) & 0xffff) << 16)))

#define VOP_WIN1_DSP_INFO_VAL(hsize, vsize) \
  ((uint32_t)(((hsize) & 0x1fff) | (((vsize) & 0x1fff) << 16)))

#define VOP_WIN1_DSP_ST_VAL(hst, vst) \
  ((uint32_t)(((hst) & 0x1fff) | (((vst) & 0x1fff) << 16)))

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_vop_s
{
  struct fb_vtable_s vtable;
  struct fb_planeinfo_s planeinfo;
  struct fb_videoinfo_s videoinfo;
  FAR void *base;     /* VOP MMIO base */
  FAR void *grf;      /* GRF MMIO base */
  int irq;
  spinlock_t lock;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3506_vop_s g_rk3506_vop;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3506_vop_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                   FAR struct fb_videoinfo_s *vinfo);
static int rk3506_vop_getplaneinfo(FAR struct fb_vtable_s *vtable,
                                   int planeno,
                                   FAR struct fb_planeinfo_s *pinfo);
static int rk3506_vop_setpower(FAR struct fb_vtable_s *vtable, int power);
static int rk3506_vop_getpower(FAR struct fb_vtable_s *vtable);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_vop_getvideoinfo
 ****************************************************************************/

static int rk3506_vop_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                   FAR struct fb_videoinfo_s *vinfo)
{
  FAR struct rk3506_vop_s *priv = (FAR struct rk3506_vop_s *)vtable;

  if (priv == NULL || vinfo == NULL)
    {
      return -EINVAL;
    }

  memcpy(vinfo, &priv->videoinfo, sizeof(struct fb_videoinfo_s));
  return OK;
}

/****************************************************************************
 * Name: rk3506_vop_getplaneinfo
 ****************************************************************************/

static int rk3506_vop_getplaneinfo(FAR struct fb_vtable_s *vtable,
                                   int planeno,
                                   FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct rk3506_vop_s *priv = (FAR struct rk3506_vop_s *)vtable;

  if (priv == NULL || planeno != 0 || pinfo == NULL)
    {
      return -EINVAL;
    }

  memcpy(pinfo, &priv->planeinfo, sizeof(struct fb_planeinfo_s));
  return OK;
}

/****************************************************************************
 * Name: rk3506_vop_setpower
 ****************************************************************************/

static int rk3506_vop_setpower(FAR struct fb_vtable_s *vtable, int power)
{
  FAR struct rk3506_vop_s *priv = (FAR struct rk3506_vop_s *)vtable;
  irqstate_t flags;
  uint32_t val;

  if (priv == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&priv->lock);

  val = getreg32(priv->base + VOP_DSP_CTRL0);
  if (power > 0)
    {
      val |= VOP_DSP_CTRL0_RGB_EN;
    }
  else
    {
      val &= ~VOP_DSP_CTRL0_RGB_EN;
    }

  putreg32(val, priv->base + VOP_DSP_CTRL0);

  /* Commit register changes */

  putreg32(VOP_REG_CFG_TRIGGER, priv->base + VOP_REG_CFG_DONE);

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}

/****************************************************************************
 * Name: rk3506_vop_getpower
 ****************************************************************************/

static int rk3506_vop_getpower(FAR struct fb_vtable_s *vtable)
{
  FAR struct rk3506_vop_s *priv = (FAR struct rk3506_vop_s *)vtable;
  uint32_t val;

  if (priv == NULL)
    {
      return -EINVAL;
    }

  val = getreg32(priv->base + VOP_DSP_CTRL0);
  return (val & VOP_DSP_CTRL0_RGB_EN) ? 1 : 0;
}

/****************************************************************************
 * Name: rk3506_vop_hwinit
 *
 * Description:
 *   Configure VOP registers for ST7701S 480x854 RGB565 output via RGB
 *   interface. Caller must hold priv->lock.
 *
 ****************************************************************************/

static void rk3506_vop_hwinit(FAR struct rk3506_vop_s *priv)
{
  uint32_t val;

  /* Set WIN1 format to RGB565 and enable the window.
   * WIN1_CTRL0: bit[0] = enable, bits[4:1] = format
   */

  putreg32(VOP_WIN1_CTRL0_EN |
           (VOP_FMT_RGB565 << VOP_WIN1_FMT_SHIFT),
           priv->base + VOP_WIN1_CTRL0);

  /* WIN1 virtual stride (pixels * bytes_per_pixel) */

  putreg32(RK3506_VOP_STRIDE / (RK3506_VOP_BPP / 8),
           priv->base + VOP_WIN1_VIR);

  /* WIN1 DMA start address (must be set after fbmem is allocated) */

  if (priv->planeinfo.fbmem == NULL)
    {
      gerr("ERROR: framebuffer not allocated\n");
      return;
    }

  putreg32((uint32_t)(uintptr_t)priv->planeinfo.fbmem,
           priv->base + VOP_WIN1_MST);

  /* WIN1 display size */

  putreg32(VOP_WIN1_DSP_INFO_VAL(RK3506_VOP_XRES, RK3506_VOP_YRES),
           priv->base + VOP_WIN1_DSP_INFO);

  /* WIN1 display start position (0, 0) */

  putreg32(VOP_WIN1_DSP_ST_VAL(0, 0), priv->base + VOP_WIN1_DSP_ST);

  /* WIN1 alpha: global alpha = 0xff, no pre-multiplied alpha */

  putreg32(0xff, priv->base + VOP_WIN1_ALPHA_CTRL);

  /* Display timing: horizontal */

  putreg32(VOP_HTOTAL_HS_END(RK3506_VOP_HSYNC, RK3506_VOP_HTOTAL),
           priv->base + VOP_DSP_HTOTAL_HS_END);

  putreg32(VOP_HACT_ST_END(RK3506_VOP_HSYNC + RK3506_VOP_HBP,
                            RK3506_VOP_HACT),
           priv->base + VOP_DSP_HACT_ST_END);

  /* Display timing: vertical */

  putreg32(VOP_VTOTAL_VS_END(RK3506_VOP_VSYNC, RK3506_VOP_VTOTAL),
           priv->base + VOP_DSP_VTOTAL_VS_END);

  putreg32(VOP_VACT_ST_END(RK3506_VOP_VSYNC + RK3506_VOP_VBP,
                            RK3506_VOP_VACT),
           priv->base + VOP_DSP_VACT_ST_END);

  /* Background color (black) */

  putreg32(0x00000000, priv->base + VOP_DSP_BG);

  /* DSP_CTRL2: enable layer select, no dithering.
   *
   * Per Linux rk3366_lit_ctrl_data (rockchip_vop_reg.c):
   *   dsp_layer_sel = DSP_CTRL2[3]  (0=layer 0 / win0, 1=layer 1 / win1)
   *   overlay_mode  = DSP_CTRL2[4]
   *
   * We select layer 1 (win1) as the primary.
   */

  putreg32(0x00000008, priv->base + VOP_DSP_CTRL2);  /* DSP_CTRL2[3] = 1 */

  /* SYS_CTRL0/SYS_CTRL1/SYS_CTRL2: defaults (no specific fields used by
   * the RK3366_LIT ctrl data for rk3506; left at 0).
   */

  putreg32(0x00000000, priv->base + VOP_SYS_CTRL0);
  putreg32(0x00000000, priv->base + VOP_SYS_CTRL1);
  putreg32(0x00000000, priv->base + VOP_SYS_CTRL2);

  /* DSP_CTRL0: enable RGB interface output */

  putreg32(VOP_DSP_CTRL0_RGB_EN, priv->base + VOP_DSP_CTRL0);

  /* Configure GRF_SOC_CON2 for RGB interface.
   *
   * Per Linux kernel rk3506_rgb_enable() (drivers/gpu/drm/rockchip/rockchip_rgb.c):
   *   regmap_write(grf, RK3506_GRF_SOC_CON2,
   *                RK3506_GRF_VOP_DATA_BYPASS(bypass ? 0x3 : 0x0));
   *   regmap_write(grf, RK3506_GRF_SOC_CON2,
   *                RK3506_GRF_VOP_DLL_SEL(0x10));  // for RGB output
   *
   * HIWORD_UPDATE pattern: bits[31:16] are write enable, bits[15:0] are data.
   *   reg = (reg & ~mask) | (data & mask)
   *
   * DLL = 0x10 (4-bit value at bits[10:4]) for RGB.
   * Bypass = 0x0 (data path active).
   */

  /* Step 1: set data bypass = 0 (active) */

  putreg32(GRF_HIWORD_UPDATE(0, GRF_SOC_CON2_BYPASS_MASK,
                             GRF_SOC_CON2_BYPASS_SHIFT),
           priv->grf + GRF_SOC_CON2);

  /* Step 2: set DLL select = 0x10 (RGB) */

  putreg32(GRF_HIWORD_UPDATE(GRF_SOC_CON2_DLL_SEL_RGB,
                             GRF_SOC_CON2_DLL_SEL_MASK,
                             GRF_SOC_CON2_DLL_SEL_SHIFT),
           priv->grf + GRF_SOC_CON2);

  /* Commit all register writes */

  putreg32(VOP_REG_CFG_TRIGGER, priv->base + VOP_REG_CFG_DONE);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3506_vop_register
 ****************************************************************************/

int rk3506_vop_register(void)
{
  FAR struct rk3506_vop_s *priv = &g_rk3506_vop;
  int ret;

  memset(priv, 0, sizeof(*priv));
  spin_lock_init(&priv->lock);

  priv->base = (FAR void *)RK3506_VOP_ADDR;
  priv->grf  = (FAR void *)RK3506_GRF_ADDR;
  priv->irq  = RK3506_IRQ_VOP;

  /* Setup video info */

  priv->videoinfo.fmt      = FB_FMT_RGB16_565;
  priv->videoinfo.xres     = RK3506_VOP_XRES;
  priv->videoinfo.yres     = RK3506_VOP_YRES;
  priv->videoinfo.nplanes  = 1;

  /* Setup plane info */

  priv->planeinfo.bpp          = RK3506_VOP_BPP;
  priv->planeinfo.stride       = RK3506_VOP_STRIDE;
  priv->planeinfo.xres_virtual = RK3506_VOP_XRES;
  priv->planeinfo.yres_virtual = RK3506_VOP_YRES;
  priv->planeinfo.fblen        = RK3506_VOP_FBSIZE;
  priv->planeinfo.fbmem = kmm_zalloc(RK3506_VOP_FBSIZE);
  if (priv->planeinfo.fbmem == NULL)
    {
      gerr("ERROR: Failed to allocate framebuffer: %d bytes\n",
           RK3506_VOP_FBSIZE);
      return -ENOMEM;
    }

  /* Setup vtable */

  priv->vtable.getvideoinfo = rk3506_vop_getvideoinfo;
  priv->vtable.getplaneinfo = rk3506_vop_getplaneinfo;
  priv->vtable.setpower     = rk3506_vop_setpower;
  priv->vtable.getpower     = rk3506_vop_getpower;

  /* Initialize VOP hardware */

  rk3506_vop_hwinit(priv);

  /* Register as /dev/fb0 */

  ret = fb_register_device(0, 0, &priv->vtable);
  if (ret < 0)
    {
      gerr("ERROR: fb_register_device failed: %d\n", ret);
      kmm_free(priv->planeinfo.fbmem);
      return ret;
    }

  ginfo("RK3506 VOP registered: %dx%d RGB565, fb=%p\n",
        RK3506_VOP_XRES, RK3506_VOP_YRES, priv->planeinfo.fbmem);
  return OK;
}
