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
 * Running an app spawns `qjs <path>` detached so the LVGL UI and the
 * NSH console stay responsive; console.log lands on the syslog.
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

#include "hd_rk3506_qapp.h"

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

/* Spawn `qjs <path>` detached.  Returns 0 on success. */

int qapp_run(const char *path)
{
  char cmd[QAPP_PATH_LEN + 16];

  snprintf(cmd, sizeof(cmd), "qjs %s &", path);
  return system(cmd) == 0 ? 0 : -1;
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
