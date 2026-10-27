/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_qapp.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QuickJS "quick app" manager for the HD-RK3506-EVM.
 *
 * Layout on storage:
 *   /data/apps/<name>/app.js        user-installed app entry point
 *   /data/apps/<name>/manifest.json optional { "name": "...", "icon": ".." }
 *   /etc/apps/<name>.js             built-in apps baked into the romfs
 *
 * Install paths:
 *   - adb push myapp/ /data/apps/myapp/           (host side)
 *   - qapp install <url>                          (curl a .js file)
 *   - qapp run <name> / qapp del <name> / qapp list
 *
 * Running an app starts an in-process QuickJS runtime ON THE LVGL
 * TASK: the JS event pump is an lv_timer, so JS code executes
 * synchronously with LVGL and can draw via the `lv` global (see
 * hd_rk3506_lv_binding.c).  The app gets a fresh screen; a long BACK
 * press (button 2) stops it and returns to the home screen.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include <debug.h>

#include <lvgl/lvgl.h>

#include "hd_rk3506_qapp.h"
#include "hd_rk3506.h"
#include "hd_rk3506_qjs_rt.h"

#if defined(CONFIG_INTERPRETERS_QUICKJS) && defined(CONFIG_HD_RK3506_UI)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define QAPP_DATA_DIR   "/data/apps"
#define QAPP_ROMFS_DIR  "/etc/apps"
#define QAPP_ENTRY      "app.js"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool qapp_is_dir(const char *path)
{
  struct stat st;

  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool qapp_is_js(const char *name)
{
  size_t len = strlen(name);

  return len > 3 && strcmp(&name[len - 3], ".js") == 0;
}

/* Scan one directory; user apps are <dir>/<name>/app.js, romfs apps are
 * flat <dir>/<name>.js files.
 */

static int qapp_scan_dir(const char *dir, bool flat, bool builtin,
                         struct qapp_info_s *apps, int count, int max)
{
  DIR *dp;
  struct dirent *de;

  dp = opendir(dir);
  if (dp == NULL)
    {
      return count;
    }

  while (count < max && (de = readdir(dp)) != NULL)
    {
      struct qapp_info_s *app = &apps[count];
      const char *name = de->d_name;

      if (name[0] == '.')
        {
          continue;
        }

      memset(app, 0, sizeof(*app));
      app->builtin = builtin;

      if (flat)
        {
          if (!qapp_is_js(name))
            {
              continue;
            }

          strlcpy(app->name, name, QAPP_NAME_LEN);
          /* strip .js for the display name */
          app->name[strlen(app->name) - 3] = '\0';
          snprintf(app->path, QAPP_PATH_LEN, "%s/%s", dir, name);
        }
      else
        {
          char entry[QAPP_PATH_LEN];

          snprintf(entry, sizeof(entry), "%s/%s/%s", dir, name,
                   QAPP_ENTRY);
          if (access(entry, R_OK) != 0)
            {
              continue;
            }

          strlcpy(app->name, name, QAPP_NAME_LEN);
          strlcpy(app->path, entry, QAPP_PATH_LEN);
        }

      count++;
    }

  closedir(dp);
  return count;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int qapp_list(struct qapp_info_s *apps, int max)
{
  int count = 0;

  count = qapp_scan_dir(QAPP_ROMFS_DIR, true, true, apps, count, max);
  count = qapp_scan_dir(QAPP_DATA_DIR, false, false, apps, count, max);
  return count;
}

/****************************************************************************
 * In-process app runner.  One app at a time: starting a second app
 * stops the first.  The app draws on a fresh screen which replaces the
 * home screen for the app's lifetime.
 ****************************************************************************/

static struct
{
  struct hd_qjs_app_s *js;       /* QuickJS runtime handle            */
  lv_obj_t            *screen;   /* app's screen                      */
  lv_obj_t            *prev;     /* screen to restore on stop         */
  lv_timer_t          *pump;     /* JS job pump (lv_timer)            */
} g_running;

static void qapp_pump_cb(lv_timer_t *t)
{
  (void)t;

  if (g_running.js != NULL)
    {
      hd_qjs_app_pump(g_running.js);
    }
}

/* ESC while an app runs stops the app.  This is registered on the
 * board's back-key hook rather than as an LV_EVENT_KEY on the app
 * screen: the LVGL keypad driver delivers ESC only to the focused
 * object, so once an app's own widgets take group focus the screen
 * handler would never see the key.  The hook runs from lv_async_call()
 * in the LVGL timer task, i.e. outside any event dispatch, so
 * qapp_stop() may delete the screen it was called from.
 */

static void qapp_back_cb(FAR void *arg)
{
  (void)arg;
  qapp_stop();
}

int qapp_run(const char *path)
{
  lv_group_t *group = lv_group_get_default();

  /* One app at a time. */

  qapp_stop();

  /* Fresh screen for the app, remember where to return. */

  g_running.prev   = lv_screen_active();
  g_running.screen = lv_obj_create(NULL);
  if (g_running.screen == NULL)
    {
      return -1;
    }

  /* Hide the home screen so its group widgets are skipped by focus
   * traversal while the app runs.
   */

  lv_obj_add_flag(g_running.prev, LV_OBJ_FLAG_HIDDEN);

  /* Load the app screen BEFORE starting JS: the app's script calls
   * lv.screenActive() during evaluation, and it must see the app
   * screen, not the (now hidden) home screen.
   */

  lv_screen_load(g_running.screen);

  /* ESC while this app runs stops it; the handler shadows the UI's
   * default back handler until qapp_stop() unregisters it.
   */

  if (hd_rk3506_back_register(qapp_back_cb, NULL) < 0)
    {
      _err("qapp: failed to hook the back key\n");
    }

  if (group != NULL)
    {
      lv_group_add_obj(group, g_running.screen);
      lv_group_focus_obj(g_running.screen);
    }

  g_running.js = hd_qjs_app_start(path);
  if (g_running.js == NULL)
    {
      _err("qapp: failed to start %s\n", path);
      hd_rk3506_back_unregister(qapp_back_cb);
      lv_obj_remove_flag(g_running.prev, LV_OBJ_FLAG_HIDDEN);
      lv_screen_load(g_running.prev);
      lv_obj_delete(g_running.screen);
      g_running.screen = NULL;
      g_running.prev   = NULL;
      return -1;
    }

  /* Pump JS jobs from the LVGL task. */

  g_running.pump = lv_timer_create(qapp_pump_cb, 33, NULL);

  _info("qapp: %s running\n", path);
  return 0;
}

void qapp_stop(void)
{
  lv_group_t *group = lv_group_get_default();

  if (g_running.js == NULL && g_running.screen == NULL)
    {
      return;
    }

  if (g_running.pump != NULL)
    {
      lv_timer_delete(g_running.pump);
      g_running.pump = NULL;
    }

  /* Restore the home screen FIRST so it becomes the active screen again
   * before we delete the app screen.  Deleting the active screen would
   * leave the display with no active screen until the next screen_load.
   */

  if (g_running.prev != NULL)
    {
      lv_obj_remove_flag(g_running.prev, LV_OBJ_FLAG_HIDDEN);
      lv_screen_load(g_running.prev);
      g_running.prev = NULL;
    }

  /* Delete the app's LVGL screen: this fires LV_EVENT_DELETE on every
   * widget, releasing the JS callbacks they hold, so no trampoline can
   * fire into a freed JS context afterwards.  lv_obj_delete() handles
   * group removal and indev focus reset internally.
   */

  if (g_running.screen != NULL)
    {
      lv_obj_delete(g_running.screen);
      g_running.screen = NULL;
    }

  if (g_running.js != NULL)
    {
      hd_qjs_app_stop(g_running.js);
      g_running.js = NULL;
    }

  /* Hand the back key back to the UI (its "return to home" handler). */

  hd_rk3506_back_unregister(qapp_back_cb);

  _info("qapp: stopped\n");
}

bool qapp_active(void)
{
  return g_running.js != NULL;
}

/* Delete a user app directory (recursive, best effort).  Romfs apps are
 * read-only and rejected.
 */

int qapp_delete(const char *name)
{
  char path[QAPP_PATH_LEN];
  DIR *dp;
  struct dirent *de;

  if (strchr(name, '/') != NULL || strcmp(name, ".") == 0 ||
      strcmp(name, "..") == 0)
    {
      return -EINVAL;
    }

  snprintf(path, sizeof(path), "%s/%s", QAPP_DATA_DIR, name);
  if (!qapp_is_dir(path))
    {
      return -ENOENT;
    }

  dp = opendir(path);
  if (dp != NULL)
    {
      while ((de = readdir(dp)) != NULL)
        {
          char sub[QAPP_PATH_LEN];

          if (de->d_name[0] == '.')
            {
              continue;
            }

          snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
          unlink(sub);
        }

      closedir(dp);
    }

  return rmdir(path);
}

/* Install a single .js fetched from an URL into /data/apps/<name>/.
 * The name is derived from the URL basename (foo.js -> foo).
 */

int qapp_install_url(const char *url)
{
  char name[QAPP_NAME_LEN];
  char dir[QAPP_PATH_LEN];
  char dest[QAPP_PATH_LEN];
  char cmd[256];
  const char *base;
  const char *dot;
  size_t len;
  int ret;

  base = strrchr(url, '/');
  base = (base != NULL) ? base + 1 : url;
  dot  = strrchr(base, '.');
  if (dot == NULL || strcmp(dot, ".js") != 0)
    {
      return -EINVAL;
    }

  len = (size_t)(dot - base);
  if (len == 0 || len >= QAPP_NAME_LEN)
    {
      return -EINVAL;
    }

  memcpy(name, base, len);
  name[len] = '\0';

  mkdir(QAPP_DATA_DIR, 0777);
  snprintf(dir, sizeof(dir), "%s/%s", QAPP_DATA_DIR, name);
  mkdir(dir, 0777);

  snprintf(dest, sizeof(dest), "%s/%s", dir, QAPP_ENTRY);
  snprintf(cmd, sizeof(cmd), "curl -fsSL -o %s %s", dest, url);
  ret = system(cmd);
  if (ret != 0)
    {
      unlink(dest);
      rmdir(dir);
      return -EIO;
    }

  return 0;
}

#endif /* CONFIG_INTERPRETERS_QUICKJS && CONFIG_HD_RK3506_UI */
