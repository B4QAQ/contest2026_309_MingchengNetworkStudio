/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_qapp.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QuickJS quick-app manager API shared by the LVGL apps page and the
 * NSH "qapp" command.  See hd_rk3506_qapp.c for the storage layout.
 *
 ****************************************************************************/

#ifndef __HD_RK3506_QAPP_H
#define __HD_RK3506_QAPP_H

#include <stdbool.h>

#define QAPP_MAX        16
#define QAPP_NAME_LEN   32
#define QAPP_PATH_LEN   96

struct qapp_info_s
{
  char name[QAPP_NAME_LEN];
  char path[QAPP_PATH_LEN];
  bool builtin;
};

int qapp_list(struct qapp_info_s *apps, int max);
int qapp_run(const char *path);
int qapp_delete(const char *name);
int qapp_install_url(const char *url);

#endif /* __HD_RK3506_QAPP_H */
