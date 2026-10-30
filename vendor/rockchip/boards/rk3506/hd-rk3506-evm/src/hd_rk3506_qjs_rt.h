/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_qjs_rt.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QuickJS runtime wrapper for the quick-app system.  Creates a JS
 * context with std/os modules + the LVGL binding, evaluates an app
 * file, and provides a pump that drains pending jobs.  Everything runs
 * in the caller's task context (the LVGL task), so JS executes
 * synchronously with LVGL.
 *
 ****************************************************************************/

#ifndef __HD_RK3506_QJS_RT_H
#define __HD_RK3506_QJS_RT_H

#include <stdbool.h>

struct hd_qjs_app_s;  /* opaque app handle */

/* Create a JS runtime+context, register std/os + the lv binding, and
 * evaluate the given file.  Returns NULL on failure (the JS exception
 * is logged to the syslog).
 */

struct hd_qjs_app_s *hd_qjs_app_start(const char *path);

/* Drain pending jobs (promises, etc.).  Call from the LVGL task at
 * regular intervals (e.g. from an lv_timer).
 */

void hd_qjs_app_pump(struct hd_qjs_app_s *app);

/* Destroy the app: drop LVGL callbacks, free the context+runtime.
 * The app's LVGL objects must be deleted by the caller BEFORE this.
 */

void hd_qjs_app_stop(struct hd_qjs_app_s *app);

#endif /* __HD_RK3506_QJS_RT_H */
