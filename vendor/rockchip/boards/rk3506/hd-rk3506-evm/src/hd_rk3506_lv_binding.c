/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_lv_binding.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QuickJS -> LVGL binding ("lv" global object).
 *
 *   lv.screenActive()          -> the active screen object
 *   lv.objCreate(parent)       -> plain container (parent=null: screen)
 *   lv.labelCreate(parent)     -> label widget
 *   lv.labelSetText(l, "txt")
 *   lv.buttonCreate(parent)    -> button widget
 *   lv.objSetPos(o, x, y)
 *   lv.objSetSize(o, w, h)
 *   lv.objCenter(o)
 *   lv.objSetStyleBgColor(o, 0xRRGGBB)
 *   lv.objSetStyleTextColor(o, 0xRRGGBB)
 *   lv.objAddEventCb(o, fn, lv.EVENT_CLICKED)
 *   lv.groupAddObj(o)          -> add to default group (J9 key focus)
 *   lv.groupFocusObj(o)
 *   lv.screenLoad(o)
 *   lv.setTimeout(fn, ms)      -> one-shot lv_timer
 *
 * Every JS call runs inside the LVGL task (the JS event pump is an
 * lv_timer), so no locking is needed.  Objects are passed as opaque
 * wrappers; event/timer JS callbacks are kept alive with JS_DupValue
 * and released by hd_lv_binding_cleanup() (or when the object is
 * deleted by LVGL).
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdlib.h>

#include <debug.h>

#include <quickjs.h>

#include <lvgl/lvgl.h>

#if defined(CONFIG_INTERPRETERS_QUICKJS) && defined(CONFIG_HD_RK3506_UI)

#include "hd_rk3506_lv_binding.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One outstanding JS callback owned by this module (event or timer). */

struct lvjs_cb_s
{
  JSContext         *ctx;
  JSValue            func;      /* Dup'd JS function                  */
  struct lvjs_cb_s  *next;
};

static JSClassID          g_lvobj_class_id;
static struct lvjs_cb_s  *g_cbs;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static struct lvjs_cb_s *lvjs_cb_new(JSContext *ctx, JSValueConst func)
{
  struct lvjs_cb_s *cb = malloc(sizeof(*cb));

  if (cb == NULL)
    {
      return NULL;
    }

  cb->ctx  = ctx;
  cb->func = JS_DupValue(ctx, func);
  cb->next = g_cbs;
  g_cbs    = cb;
  return cb;
}

static void lvjs_cb_free(struct lvjs_cb_s *cb)
{
  struct lvjs_cb_s **pp;

  for (pp = &g_cbs; *pp != NULL; pp = &(*pp)->next)
    {
      if (*pp == cb)
        {
          *pp = cb->next;
          JS_FreeValue(cb->ctx, cb->func);
          free(cb);
          return;
        }
    }
}

/* ---- lv_obj_t* wrapper ------------------------------------------------- */

static JSValue lvjs_wrap_obj(JSContext *ctx, lv_obj_t *obj)
{
  JSValue js = JS_NewObjectClass(ctx, g_lvobj_class_id);

  if (JS_IsException(js))
    {
      return js;
    }

  JS_SetOpaque(js, obj);
  return js;
}

static lv_obj_t *lvjs_unwrap(JSContext *ctx, JSValueConst val)
{
  return JS_GetOpaque(val, g_lvobj_class_id);
}

static lv_obj_t *lvjs_arg_obj(JSContext *ctx, JSValueConst val)
{
  lv_obj_t *obj = lvjs_unwrap(ctx, val);

  if (obj == NULL)
    {
      JS_ThrowTypeError(ctx, "not an lv object");
    }

  return obj;
}

/* ---- event trampoline -------------------------------------------------- */

static void lvjs_event_trampoline(lv_event_t *e)
{
  struct lvjs_cb_s *cb = lv_event_get_user_data(e);
  JSValue ret;

  if (cb == NULL)
    {
      return;
    }

  ret = JS_Call(cb->ctx, cb->func, JS_UNDEFINED, 0, NULL);
  if (JS_IsException(ret))
    {
      JSValue exc = JS_GetException(cb->ctx);
      const char *str = JS_ToCString(cb->ctx, exc);

      _err("lvjs event cb: %s\n", str ? str : "(exception)");
      if (str != NULL)
        {
          JS_FreeCString(cb->ctx, str);
        }

      JS_FreeValue(cb->ctx, exc);
    }

  JS_FreeValue(cb->ctx, ret);
}

static void lvjs_event_delete_trampoline(lv_event_t *e)
{
  struct lvjs_cb_s *cb = lv_event_get_user_data(e);

  if (cb != NULL)
    {
      lvjs_cb_free(cb);
    }
}

/* ---- lv.setTimeout ----------------------------------------------------- */

static void lvjs_timeout_trampoline(lv_timer_t *t)
{
  struct lvjs_cb_s *cb = t->user_data;
  JSValue ret;

  ret = JS_Call(cb->ctx, cb->func, JS_UNDEFINED, 0, NULL);
  if (JS_IsException(ret))
    {
      JSValue exc = JS_GetException(cb->ctx);
      const char *str = JS_ToCString(cb->ctx, exc);

      _err("lvjs timeout cb: %s\n", str ? str : "(exception)");
      if (str != NULL)
        {
          JS_FreeCString(cb->ctx, str);
        }

      JS_FreeValue(cb->ctx, exc);
    }

  JS_FreeValue(cb->ctx, ret);
  lv_timer_delete(t);
  lvjs_cb_free(cb);
}

static JSValue lvjs_set_timeout(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
  struct lvjs_cb_s *cb;
  lv_timer_t *t;
  int32_t ms = 0;

  (void)this_val;

  if (argc < 2 || !JS_IsFunction(ctx, argv[0]) ||
      JS_ToInt32(ctx, &ms, argv[1]) < 0)
    {
      return JS_ThrowTypeError(ctx, "setTimeout(fn, ms)");
    }

  cb = lvjs_cb_new(ctx, argv[0]);
  if (cb == NULL)
    {
      return JS_ThrowOutOfMemory(ctx);
    }

  t = lv_timer_create(lvjs_timeout_trampoline, (uint32_t)ms, cb);
  if (t == NULL)
    {
      lvjs_cb_free(cb);
      return JS_ThrowInternalError(ctx, "lv_timer_create failed");
    }

  lv_timer_set_repeat_count(t, 1);
  return JS_UNDEFINED;
}

/* ---- object helpers ---------------------------------------------------- */

static JSValue lvjs_screen_active(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
  (void)this_val;
  (void)argc;
  (void)argv;

  return lvjs_wrap_obj(ctx, lv_screen_active());
}

static JSValue lvjs_screen_load(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
  lv_obj_t *obj;

  (void)this_val;

  if (argc < 1 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL)
    {
      return JS_EXCEPTION;
    }

  lv_screen_load(obj);
  return JS_UNDEFINED;
}

typedef lv_obj_t *(*lvjs_create_fn_t)(lv_obj_t *parent);

static JSValue lvjs_create_widget(JSContext *ctx, JSValueConst *argv,
                                  lvjs_create_fn_t fn)
{
  lv_obj_t *parent = NULL;
  lv_obj_t *obj;

  if (JS_IsObject(argv[0]))
    {
      parent = lvjs_arg_obj(ctx, argv[0]);
      if (parent == NULL)
        {
          return JS_EXCEPTION;
        }
    }

  obj = fn(parent);
  if (obj == NULL)
    {
      return JS_ThrowOutOfMemory(ctx);
    }

  return lvjs_wrap_obj(ctx, obj);
}

static JSValue lvjs_obj_create(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
  (void)this_val;

  if (argc < 1)
    {
      return JS_ThrowTypeError(ctx, "objCreate(parent)");
    }

  return lvjs_create_widget(ctx, argv, lv_obj_create);
}

static JSValue lvjs_label_create(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
  (void)this_val;

  if (argc < 1)
    {
      return JS_ThrowTypeError(ctx, "labelCreate(parent)");
    }

  return lvjs_create_widget(ctx, argv, lv_label_create);
}

static JSValue lvjs_button_create(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
  (void)this_val;

  if (argc < 1)
    {
      return JS_ThrowTypeError(ctx, "buttonCreate(parent)");
    }

  return lvjs_create_widget(ctx, argv, lv_button_create);
}

static JSValue lvjs_obj_delete(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
  lv_obj_t *obj;

  (void)this_val;

  if (argc < 1 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL)
    {
      return JS_EXCEPTION;
    }

  lv_obj_delete(obj);
  return JS_UNDEFINED;
}

static JSValue lvjs_label_set_text(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
  lv_obj_t *obj;
  const char *text;
  JSValue ret = JS_UNDEFINED;

  (void)this_val;

  if (argc < 2 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL)
    {
      return JS_EXCEPTION;
    }

  text = JS_ToCString(ctx, argv[1]);
  if (text == NULL)
    {
      return JS_EXCEPTION;
    }

  lv_label_set_text(obj, text);
  JS_FreeCString(ctx, text);
  return ret;
}

static JSValue lvjs_obj_set_pos(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
  lv_obj_t *obj;
  int32_t x;
  int32_t y;

  (void)this_val;

  if (argc < 3 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL ||
      JS_ToInt32(ctx, &x, argv[1]) < 0 ||
      JS_ToInt32(ctx, &y, argv[2]) < 0)
    {
      return JS_ThrowTypeError(ctx, "objSetPos(obj, x, y)");
    }

  lv_obj_set_pos(obj, x, y);
  return JS_UNDEFINED;
}

static JSValue lvjs_obj_set_size(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
  lv_obj_t *obj;
  int32_t w;
  int32_t h;

  (void)this_val;

  if (argc < 3 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL ||
      JS_ToInt32(ctx, &w, argv[1]) < 0 ||
      JS_ToInt32(ctx, &h, argv[2]) < 0)
    {
      return JS_ThrowTypeError(ctx, "objSetSize(obj, w, h)");
    }

  lv_obj_set_size(obj, w, h);
  return JS_UNDEFINED;
}

static JSValue lvjs_obj_center(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
  lv_obj_t *obj;

  (void)this_val;

  if (argc < 1 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL)
    {
      return JS_EXCEPTION;
    }

  lv_obj_center(obj);
  return JS_UNDEFINED;
}

static JSValue lvjs_set_style_bg_color(JSContext *ctx,
                                       JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
  lv_obj_t *obj;
  uint32_t rgb;

  (void)this_val;

  if (argc < 2 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL ||
      JS_ToUint32(ctx, &rgb, argv[1]) < 0)
    {
      return JS_ThrowTypeError(ctx, "objSetStyleBgColor(obj, 0xRRGGBB)");
    }

  lv_obj_set_style_bg_color(obj, lv_color_hex(rgb), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  return JS_UNDEFINED;
}

static JSValue lvjs_set_style_text_color(JSContext *ctx,
                                         JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
  lv_obj_t *obj;
  uint32_t rgb;

  (void)this_val;

  if (argc < 2 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL ||
      JS_ToUint32(ctx, &rgb, argv[1]) < 0)
    {
      return JS_ThrowTypeError(ctx, "objSetStyleTextColor(obj, 0xRRGGBB)");
    }

  lv_obj_set_style_text_color(obj, lv_color_hex(rgb), 0);
  return JS_UNDEFINED;
}

static JSValue lvjs_obj_add_event_cb(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
  struct lvjs_cb_s *cb;
  lv_obj_t *obj;
  uint32_t code;

  (void)this_val;

  if (argc < 3 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL ||
      !JS_IsFunction(ctx, argv[1]) ||
      JS_ToUint32(ctx, &code, argv[2]) < 0)
    {
      return JS_ThrowTypeError(ctx, "objAddEventCb(obj, fn, code)");
    }

  cb = lvjs_cb_new(ctx, argv[1]);
  if (cb == NULL)
    {
      return JS_ThrowOutOfMemory(ctx);
    }

  lv_obj_add_event_cb(obj, lvjs_event_trampoline, (lv_event_code_t)code,
                      cb);

  /* Release the JS function when the object is deleted by LVGL so the
   * callback list cannot leak (object delete fires LV_EVENT_DELETE).
   * The delete trampoline carries the same cb pointer and frees it.
   */

  lv_obj_add_event_cb(obj, lvjs_event_delete_trampoline,
                      LV_EVENT_DELETE, cb);

  return JS_UNDEFINED;
}

static JSValue lvjs_group_add_obj(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
  lv_obj_t *obj;
  lv_group_t *group = lv_group_get_default();

  (void)this_val;

  if (argc < 1 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL)
    {
      return JS_EXCEPTION;
    }

  if (group != NULL)
    {
      lv_group_add_obj(group, obj);
    }

  return JS_UNDEFINED;
}

static JSValue lvjs_group_focus_obj(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
  lv_obj_t *obj;
  lv_group_t *group = lv_group_get_default();

  (void)this_val;

  if (argc < 1 || (obj = lvjs_arg_obj(ctx, argv[0])) == NULL)
    {
      return JS_EXCEPTION;
    }

  if (group != NULL)
    {
      lv_group_focus_obj(obj);
    }

  return JS_UNDEFINED;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int hd_lv_binding_init(JSContext *ctx)
{
  static const JSCFunctionListEntry lv_funcs[] =
  {
    JS_CFUNC_DEF("screenActive",       0, lvjs_screen_active),
    JS_CFUNC_DEF("screenLoad",         1, lvjs_screen_load),
    JS_CFUNC_DEF("objCreate",          1, lvjs_obj_create),
    JS_CFUNC_DEF("objDelete",          1, lvjs_obj_delete),
    JS_CFUNC_DEF("labelCreate",        1, lvjs_label_create),
    JS_CFUNC_DEF("labelSetText",       2, lvjs_label_set_text),
    JS_CFUNC_DEF("buttonCreate",       1, lvjs_button_create),
    JS_CFUNC_DEF("objSetPos",          3, lvjs_obj_set_pos),
    JS_CFUNC_DEF("objSetSize",         3, lvjs_obj_set_size),
    JS_CFUNC_DEF("objCenter",          1, lvjs_obj_center),
    JS_CFUNC_DEF("objSetStyleBgColor", 2, lvjs_set_style_bg_color),
    JS_CFUNC_DEF("objSetStyleTextColor", 2, lvjs_set_style_text_color),
    JS_CFUNC_DEF("objAddEventCb",      3, lvjs_obj_add_event_cb),
    JS_CFUNC_DEF("groupAddObj",        1, lvjs_group_add_obj),
    JS_CFUNC_DEF("groupFocusObj",      1, lvjs_group_focus_obj),
    JS_CFUNC_DEF("setTimeout",         2, lvjs_set_timeout),
  };

  JSValue global;
  JSValue lv;
  JSValue proto;

  static const JSClassDef lvobj_class_def =
  {
    "LVObj",
    /* No finalizer: the lv_obj_t* is owned by LVGL, not by JS. */
  };

  JS_NewClassID(&g_lvobj_class_id);
  JS_NewClass(JS_GetRuntime(ctx), g_lvobj_class_id, &lvobj_class_def);

  proto = JS_NewObject(ctx);
  JS_SetClassProto(ctx, g_lvobj_class_id, proto);

  lv = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, lv, lv_funcs,
                             sizeof(lv_funcs) / sizeof(lv_funcs[0]));

  /* Event code constants. */

  JS_SetPropertyStr(ctx, lv, "EVENT_CLICKED",
                    JS_NewUint32(ctx, LV_EVENT_CLICKED));
  JS_SetPropertyStr(ctx, lv, "EVENT_PRESSED",
                    JS_NewUint32(ctx, LV_EVENT_PRESSED));
  JS_SetPropertyStr(ctx, lv, "EVENT_RELEASED",
                    JS_NewUint32(ctx, LV_EVENT_RELEASED));
  JS_SetPropertyStr(ctx, lv, "EVENT_KEY",
                    JS_NewUint32(ctx, LV_EVENT_KEY));

  global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "lv", lv);
  JS_FreeValue(ctx, global);
  return 0;
}

void hd_lv_binding_cleanup(JSContext *ctx)
{
  struct lvjs_cb_s *cb = g_cbs;
  struct lvjs_cb_s *next;

  (void)ctx;

  while (cb != NULL)
    {
      next = cb->next;
      if (cb->ctx == ctx)
        {
          JS_FreeValue(cb->ctx, cb->func);
          free(cb);
        }

      cb = next;
    }

  g_cbs = NULL;
}

#endif /* CONFIG_INTERPRETERS_QUICKJS && CONFIG_HD_RK3506_UI */
