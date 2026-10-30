/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_lv_binding.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hand-rolled QuickJS -> LVGL binding for the HD-RK3506-EVM quick-app
 * runtime.  Registers a global `lv` object in a JSContext; all calls are
 * plain synchronous LVGL calls and MUST be made from the LVGL task (the
 * runtime guarantees this - JS code executes inside lv_timer context).
 *
 ****************************************************************************/

#ifndef __HD_RK3506_LV_BINDING_H
#define __HD_RK3506_LV_BINDING_H

#include <quickjs.h>

/* Register the global `lv` object.  Returns 0 on success. */

int hd_lv_binding_init(JSContext *ctx);

/* Drop every JSValue this module still holds (event callbacks, timer
 * callbacks).  Must be called before JS_FreeContext.
 */

void hd_lv_binding_cleanup(JSContext *ctx);

#endif /* __HD_RK3506_LV_BINDING_H */
