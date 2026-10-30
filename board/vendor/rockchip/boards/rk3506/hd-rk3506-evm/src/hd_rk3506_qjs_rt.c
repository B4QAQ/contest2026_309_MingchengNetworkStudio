/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_qjs_rt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QuickJS runtime wrapper for the quick-app system.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <debug.h>

#include <quickjs.h>
#include <quickjs-libc.h>

#if defined(CONFIG_INTERPRETERS_QUICKJS) && defined(CONFIG_HD_RK3506_UI)

#include "hd_rk3506_qjs_rt.h"
#include "hd_rk3506_lv_binding.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define QJS_MEMORY_LIMIT   (8 * 1024 * 1024)   /* 8 MB per app */
#define QJS_STACK_LIMIT    (256 * 1024)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hd_qjs_app_s
{
  JSRuntime *rt;
  JSContext *ctx;
  char       path[96];
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void qjs_dump_exception(JSContext *ctx)
{
  JSValue exc = JS_GetException(ctx);
  const char *str = JS_ToCString(ctx, exc);

  if (str != NULL)
    {
      _err("qjs: %s\n", str);
      JS_FreeCString(ctx, str);
    }

  if (JS_IsObject(exc))
    {
      JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
      const char *sstr = JS_ToCString(ctx, stack);

      if (sstr != NULL)
        {
          _err("qjs stack:\n%s\n", sstr);
          JS_FreeCString(ctx, sstr);
        }

      JS_FreeValue(ctx, stack);
    }

  JS_FreeValue(ctx, exc);
}

static uint8_t *qjs_load_file(const char *path, size_t *len)
{
  FILE *f;
  uint8_t *buf;
  long size;

  f = fopen(path, "rb");
  if (f == NULL)
    {
      return NULL;
    }

  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);

  buf = malloc((size_t)size + 1);
  if (buf == NULL)
    {
      fclose(f);
      return NULL;
    }

  if (fread(buf, 1, (size_t)size, f) != (size_t)size)
    {
      free(buf);
      fclose(f);
      return NULL;
    }

  fclose(f);
  buf[size] = '\0';
  *len = (size_t)size;
  return buf;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct hd_qjs_app_s *hd_qjs_app_start(const char *path)
{
  struct hd_qjs_app_s *app;
  JSValue ret;
  uint8_t *buf;
  size_t len;

  app = calloc(1, sizeof(*app));
  if (app == NULL)
    {
      return NULL;
    }

  strlcpy(app->path, path, sizeof(app->path));

  app->rt = JS_NewRuntime();
  if (app->rt == NULL)
    {
      free(app);
      return NULL;
    }

  JS_SetMemoryLimit(app->rt, QJS_MEMORY_LIMIT);
  JS_SetMaxStackSize(app->rt, QJS_STACK_LIMIT);

  app->ctx = JS_NewContext(app->rt);
  if (app->ctx == NULL)
    {
      JS_FreeRuntime(app->rt);
      free(app);
      return NULL;
    }

  js_std_add_helpers(app->ctx, 0, NULL);
  js_init_module_std(app->ctx, "std");
  js_init_module_os(app->ctx, "os");

  if (hd_lv_binding_init(app->ctx) < 0)
    {
      _err("qjs: lv binding init failed\n");
      goto fail;
    }

  buf = qjs_load_file(path, &len);
  if (buf == NULL)
    {
      _err("qjs: cannot read %s\n", path);
      goto fail;
    }

  ret = JS_Eval(app->ctx, (const char *)buf, len, path,
                JS_EVAL_TYPE_GLOBAL);
  free(buf);

  if (JS_IsException(ret))
    {
      qjs_dump_exception(app->ctx);
      JS_FreeValue(app->ctx, ret);
      goto fail;
    }

  JS_FreeValue(app->ctx, ret);

  /* Drain any jobs the top-level code scheduled (promises etc.). */

  hd_qjs_app_pump(app);
  return app;

fail:
  JS_FreeContext(app->ctx);
  JS_FreeRuntime(app->rt);
  free(app);
  return NULL;
}

void hd_qjs_app_pump(struct hd_qjs_app_s *app)
{
  JSContext *ctx1;
  int err;

  if (app == NULL)
    {
      return;
    }

  for (; ; )
    {
      err = JS_ExecutePendingJob(app->rt, &ctx1);
      if (err <= 0)
        {
          if (err < 0)
            {
              qjs_dump_exception(ctx1 != NULL ? ctx1 : app->ctx);
            }

          break;
        }
    }
}

void hd_qjs_app_stop(struct hd_qjs_app_s *app)
{
  if (app == NULL)
    {
      return;
    }

  /* Drop all JS callbacks the LVGL binding still holds (event handlers,
   * pending setTimeout callbacks) BEFORE freeing the context - the
   * trampoline must never be called with a dead context.
   */

  hd_lv_binding_cleanup(app->ctx);

  JS_FreeContext(app->ctx);
  JS_FreeRuntime(app->rt);
  free(app);
}

#endif /* CONFIG_INTERPRETERS_QUICKJS && CONFIG_HD_RK3506_UI */
