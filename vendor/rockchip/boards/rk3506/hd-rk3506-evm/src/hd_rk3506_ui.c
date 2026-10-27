/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_ui.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HD-RK3506-EVM home screen for the 428x142 landscape NV3007 panel.
 *
 * Dark theme:
 *   +----------------------------------------------+-------------+
 *   |  CPU card      |  MEM card                   |  (home)      |
 *   +----------------+-----------------------------+  (cmd)       |
 *   |  NETWORK card  |  USB card                   |  (test)      |
 *   +----------------------------------------------+  (info)      |
 *                                                     +-----------+
 *
 *   - Left: four live status cards with accent-colored title stripes,
 *     refreshed by an LVGL timer once per second.
 *   - Right: icon menu in the LVGL default group - the J9 buttons walk
 *     it (NEXT = move focus, ENTER = activate).  Focused widget shows
 *     a bright outline.
 *   - cmd:  textarea + keyboard, runs the line with system().
 *   - test: one-shot hardware smoke tests with PASS/FAIL results.
 *   - info: static system facts (chip, kernel, storage, panel, keys).
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <lvgl/lvgl.h>

#include "rk3506_gmac0.h"
#include "hd_rk3506_qapp.h"

#if defined(CONFIG_GRAPHICS_LVGL) && defined(CONFIG_HD_RK3506_UI)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define UI_SCR_W        428
#define UI_SCR_H        142

#define UI_MENU_W       56
#define UI_CONTENT_W    (UI_SCR_W - UI_MENU_W - 10)

#define UI_CARD_W       ((UI_CONTENT_W - 8) / 2)
#define UI_CARD_H       ((UI_SCR_H - 16) / 2)

#define UI_REFRESH_MS   1000

#define UI_PAGE_HOME    0
#define UI_PAGE_CMD     1
#define UI_PAGE_APPS    2
#define UI_PAGE_INFO    3
#define UI_NPAGES       4

/* Palette */

#define UI_COL_BG       0x14171d
#define UI_COL_CARD     0x22262f
#define UI_COL_CARD_BR  0x3a4150
#define UI_COL_TEXT     0xe8eaf0
#define UI_COL_DIM      0x8b93a3
#define UI_COL_ACCENT0  0xffa040    /* CPU    - orange */
#define UI_COL_ACCENT1  0x4cd97b    /* MEM    - green  */
#define UI_COL_ACCENT2  0x40a0ff    /* NET    - blue   */
#define UI_COL_ACCENT3  0xc060ff    /* USB    - purple */
#define UI_COL_MENU_BG  0x1b1f27
#define UI_COL_BTN      0x2b313d
#define UI_COL_FOCUS    0xffd060    /* focus outline  */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *g_pages[UI_NPAGES];
static lv_obj_t *g_card_lbl[4];     /* value labels of the 4 cards */

static lv_obj_t *g_cmd_result;

static lv_obj_t *g_info_uptime;
static lv_obj_t *g_apps_list;

static void ui_apps_fill(void)
{
  int i;

  lv_obj_clean(g_apps_list);
  g_apps_count = qapp_list(g_apps, QAPP_MAX);

  if (g_apps_count == 0)
    {
      lv_obj_t *lbl = ui_make_label(g_apps_list,
                                    "no apps - adb push / qapp install",
                                    &lv_font_montserrat_12, UI_COL_DIM);
      lv_obj_set_pos(lbl, 0, 0);
      return;
    }

  for (i = 0; i < g_apps_count; i++)
    {
      lv_obj_t *card;
      lv_obj_t *icon;
      lv_obj_t *name;
      lv_obj_t *btn;
      lv_obj_t *lbl;
      int y = i * 36;

      /* Card container */

      card = lv_obj_create(g_apps_list);
      lv_obj_set_size(card, UI_CONTENT_W, 32);
      lv_obj_set_pos(card, 0, y);
      lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_CARD), 0);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(card, 6, 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_color(card, lv_color_hex(UI_COL_CARD_BR),
                                    0);
      lv_obj_set_style_pad_all(card, 2, 0);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

      /* App icon: colored circle with first letter */

      icon = lv_obj_create(card);
      lv_obj_set_size(icon, 26, 26);
      lv_obj_set_pos(icon, 2, 1);
      lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(icon,
                                lv_color_hex(g_apps[i].builtin ?
                                             UI_COL_ACCENT2 :
                                             UI_COL_ACCENT1),
                                0);
      lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(icon, 0, 0);
      lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);

      lbl = lv_label_create(icon);
      {
        char letter[2] = { g_apps[i].name[0], '\0' };

        /* Uppercase the first letter for the icon */

        if (letter[0] >= 'a' && letter[0] <= 'z')
          {
            letter[0] -= 32;
          }

        lv_label_set_text(lbl, letter);
      }
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
      lv_obj_center(lbl);

      /* App name */

      name = ui_make_label(card, g_apps[i].name, &lv_font_montserrat_12,
                           g_apps[i].builtin ? UI_COL_DIM : UI_COL_TEXT);
      lv_obj_set_pos(name, 34, 8);

      /* Run button */

      btn = lv_button_create(card);
      lv_obj_set_size(btn, 40, 24);
      lv_obj_set_pos(btn, UI_CONTENT_W - 96, 2);
      ui_style_button(btn);
      lv_obj_add_event_cb(btn, ui_apps_run_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      lbl = lv_label_create(btn);
      lv_label_set_text(lbl, LV_SYMBOL_PLAY);
      lv_obj_center(lbl);
      lv_group_add_obj(lv_group_get_default(), btn);

      /* Delete button (user apps only) */

      if (!g_apps[i].builtin)
        {
          btn = lv_button_create(card);
          lv_obj_set_size(btn, 40, 24);
          lv_obj_set_pos(btn, UI_CONTENT_W - 50, 2);
          ui_style_button(btn);
          lv_obj_add_event_cb(btn, ui_apps_del_cb, LV_EVENT_CLICKED,
                              (void *)(intptr_t)i);
          lbl = lv_label_create(btn);
          lv_label_set_text(lbl, LV_SYMBOL_TRASH);
          lv_obj_center(lbl);
          lv_group_add_obj(lv_group_get_default(), btn);
        }
    }
}

static void ui_build_apps(lv_obj_t *page);

/****************************************************************************
 * Private Functions - styling helpers
 ****************************************************************************/

static void ui_style_screen(lv_obj_t *obj)
{
  lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COL_BG), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(obj, 3, 0);
  lv_obj_set_style_border_width(obj, 0, 0);

  /* A 428x142 fixed UI must never grow scrollbars - they would steal
   * pixels from the cards.  Kill scrolling on every container.
   */

  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static void ui_style_card(lv_obj_t *obj, uint32_t accent)
{
  lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COL_CARD), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, 6, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, lv_color_hex(UI_COL_CARD_BR), 0);
  /* Accent stripe on the left edge */

  lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_width(obj, 3, 0);
  lv_obj_set_style_border_color(obj, lv_color_hex(accent), 0);
  lv_obj_set_style_pad_all(obj, 4, 0);
  lv_obj_set_style_pad_left(obj, 8, 0);
}

static void ui_style_button(lv_obj_t *btn)
{
  lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_BTN), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(UI_COL_CARD_BR), 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(UI_COL_TEXT), 0);

  /* Pressed: slightly brighter */

  lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_CARD_BR),
                            LV_STATE_PRESSED);

  /* Focused (J9 NEXT key): bright outline so the user sees the cursor */

  lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_color(btn, lv_color_hex(UI_COL_FOCUS),
                                 LV_STATE_FOCUSED);
  lv_obj_set_style_outline_pad(btn, 1, LV_STATE_FOCUSED);
}

static lv_obj_t *ui_make_label(lv_obj_t *parent, const char *text,
                               const lv_font_t *font, uint32_t color)
{
  lv_obj_t *lbl = lv_label_create(parent);

  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
  return lbl;
}

/****************************************************************************
 * Private Functions - data gathering
 ****************************************************************************/

/* CPU card: line 1 = total busy ratio from /proc/cpuload (" 12.3%\n",
 * one line per CPU; single-core build -> one line), line 2 = uptime.
 * procfs sampling is cumulative since boot, which is exactly what a
 * glanceable card wants.
 */

static void ui_fmt_cpu(char *buf, size_t len)
{
  struct timespec ts;
  unsigned long s;
  char line[24];
  int fd;
  int n;

  strlcpy(line, "n/a", sizeof(line));
  fd = open("/proc/cpuload", O_RDONLY);
  if (fd >= 0)
    {
      n = read(fd, line, sizeof(line) - 1);
      close(fd);
      if (n > 0)
        {
          char *nl;

          line[n] = '\0';
          nl = strchr(line, '\n');
          if (nl != NULL)
            {
              *nl = '\0';
            }
        }
      else
        {
          strlcpy(line, "n/a", sizeof(line));
        }
    }

  clock_gettime(CLOCK_MONOTONIC, &ts);
  s = (unsigned long)ts.tv_sec;
  snprintf(buf, len, "%s\nup %lu:%02lu:%02lu",
           line, s / 3600, (s / 60) % 60, s % 60);
}

/* MEM card: line 1 = heap usage percent + used KiB, line 2 = free KiB */

static void ui_fmt_mem(char *buf, size_t len)
{
  struct mallinfo mi = mallinfo();
  unsigned long total = mi.uordblks + mi.fordblks;
  unsigned int pct = (total != 0) ?
                     (unsigned int)((100ull * mi.uordblks) / total) : 0;

  snprintf(buf, len, "%u%% %uK\nfree %uK", pct,
           (unsigned)(mi.uordblks / 1024),
           (unsigned)(mi.fordblks / 1024));
}

static void ui_fmt_ip(char *buf, size_t len)
{
  struct ifreq ifr;
  int fd;
  int ok = 0;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd >= 0)
    {
      memset(&ifr, 0, sizeof(ifr));
      strlcpy(ifr.ifr_name, "eth0", IFNAMSIZ);
      if (ioctl(fd, SIOCGIFADDR, (unsigned long)(uintptr_t)&ifr) == 0)
        {
          struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;

          if (sin->sin_addr.s_addr != 0)
            {
              strlcpy(buf, inet_ntoa(sin->sin_addr), len);
              ok = 1;
            }
        }

      close(fd);
    }

  if (!ok)
    {
      strlcpy(buf, "no link", len);
    }
}

/* NETWORK card value label shows two lines: the eth0 IPv4 address and
 * the instantaneous throughput, derived from the GMAC driver's
 * cumulative byte counters sampled at the 1 Hz refresh period.
 */

static void ui_fmt_net(char *buf, size_t len, uint32_t period_ms)
{
  static uint64_t last_rx;
  static uint64_t last_tx;
  static bool primed;
  uint64_t rx = 0;
  uint64_t tx = 0;
  char ip[32];
  char rate[32];

  ui_fmt_ip(ip, sizeof(ip));

  if (rk3506_gmac0_get_stats(&rx, &tx) < 0 || rx == 0)
    {
      snprintf(buf, len, "%s\n-", ip);
      return;
    }

  if (!primed)
    {
      primed  = true;
      last_rx = rx;
      last_tx = tx;
      snprintf(buf, len, "%s\nrx - tx -", ip);
      return;
    }

  {
    uint64_t rbps = (rx - last_rx) * 1000ull / period_ms;
    uint64_t tbps = (tx - last_tx) * 1000ull / period_ms;

    last_rx = rx;
    last_tx = tx;

    if (rbps >= 1024 * 1024 || tbps >= 1024 * 1024)
      {
        snprintf(rate, sizeof(rate), "%u.%u/%u.%u MB/s",
                 (unsigned)(rbps >> 20),
                 (unsigned)(((rbps >> 10) & 0x3ff) * 10 / 1024),
                 (unsigned)(tbps >> 20),
                 (unsigned)(((tbps >> 10) & 0x3ff) * 10 / 1024));
      }
    else
      {
        snprintf(rate, sizeof(rate), "%u/%u KB/s",
                 (unsigned)(rbps >> 10), (unsigned)(tbps >> 10));
      }
  }

  snprintf(buf, len, "%s\n%s", ip, rate);
}

static void ui_fmt_usb(char *buf, size_t len)
{
  if (access("/dev/sda", F_OK) == 0)
    {
      strlcpy(buf, "disk in", len);
    }
  else
    {
      strlcpy(buf, "no disk", len);
    }
}

/****************************************************************************
 * Private Functions - card refresh
 ****************************************************************************/

static void ui_refresh_cb(lv_timer_t *t)
{
  char buf[48];

  (void)t;

  ui_fmt_cpu(buf, sizeof(buf));
  lv_label_set_text(g_card_lbl[0], buf);

  ui_fmt_mem(buf, sizeof(buf));
  lv_label_set_text(g_card_lbl[1], buf);

  ui_fmt_net(buf, sizeof(buf), UI_REFRESH_MS);
  lv_label_set_text(g_card_lbl[2], buf);

  ui_fmt_usb(buf, sizeof(buf));
  lv_label_set_text(g_card_lbl[3], buf);

  if (g_info_uptime != NULL)
    {
      struct timespec ts;
      unsigned long s;

      clock_gettime(CLOCK_MONOTONIC, &ts);
      s = (unsigned long)ts.tv_sec;
      snprintf(buf, sizeof(buf), "uptime %lu:%02lu:%02lu",
               s / 3600, (s / 60) % 60, s % 60);
      lv_label_set_text(g_info_uptime, buf);
    }
}

/****************************************************************************
 * Private Functions - pages
 ****************************************************************************/

static void ui_show_page(int idx)
{
  int i;

  for (i = 0; i < UI_NPAGES; i++)
    {
      if (i == idx)
        {
          lv_obj_remove_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (idx == UI_PAGE_HOME)
    {
      ui_refresh_cb(NULL);
    }

  if (idx == UI_PAGE_APPS)
    {
      ui_apps_fill();
    }
}

static void ui_menu_cb(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);

  ui_show_page(idx);
}

static lv_obj_t *ui_make_card(lv_obj_t *parent, int x, int y,
                              const char *title, uint32_t accent)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_t *lbl;

  lv_obj_set_size(card, UI_CARD_W, UI_CARD_H);
  lv_obj_set_pos(card, x, y);
  ui_style_card(card, accent);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lbl = ui_make_label(card, title, &lv_font_montserrat_12,
                      UI_COL_DIM);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

  lbl = ui_make_label(card, "-", &lv_font_montserrat_12,
                      UI_COL_TEXT);
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  return lbl;
}

static void ui_build_home(lv_obj_t *page)
{
  g_card_lbl[0] = ui_make_card(page, 0, 0, "CPU",
                               UI_COL_ACCENT0);
  g_card_lbl[1] = ui_make_card(page, UI_CARD_W + 8, 0, "MEM",
                               UI_COL_ACCENT1);
  g_card_lbl[2] = ui_make_card(page, 0, UI_CARD_H + 8, "NETWORK",
                               UI_COL_ACCENT2);
  g_card_lbl[3] = ui_make_card(page, UI_CARD_W + 8, UI_CARD_H + 8,
                               "USB", UI_COL_ACCENT3);
}

/* Preset command list: each row is a focusable button that runs a
 * fixed NSH command via system().  No on-screen keyboard - the four
 * buttons cannot drive one.
 */

/* Terminal page: preset NSH commands + hardware tests in one
 * scrollable list.  Commands run via system(); tests check hardware
 * directly and report PASS/FAIL.
 */

enum ui_term_entry_type_e
{
  UI_TERM_CMD,       /* Run an NSH command via system()             */
  UI_TERM_TEST       /* Run a hardware test, report PASS/FAIL       */
};

struct ui_term_entry_s
{
  const char                 *label;
  enum ui_term_entry_type_e   type;
  const char                 *cmd;      /* UI_TERM_CMD only          */
  int                       (*test)(void); /* UI_TERM_TEST: 0=PASS   */
};

/* Hardware test helpers - return 0 on PASS, -1 on FAIL. */

static int ui_test_sd(void)
{
  return access("/dev/mmcsd0", F_OK);
}

static int ui_test_usb(void)
{
  return access("/dev/sda", F_OK);
}

static int ui_test_net(void)
{
  char buf[48];

  ui_fmt_ip(buf, sizeof(buf));
  return strcmp(buf, "no link") != 0 ? 0 : -1;
}

static int ui_test_rpmsg(void)
{
  return access("/dev/rpmsg/mcu", F_OK) == 0 ||
         access("/dev/rpmsg-rpmsg-mcu0-echo", F_OK) == 0 ? 0 : -1;
}

static int ui_test_nand(void)
{
  return access("/dev/mtdblock0", F_OK);
}

static int ui_test_rtc(void)
{
  struct tm tm;
  time_t t = time(NULL);

  /* PASS if the clock is past 2024 (NTP synced). */

  if (t == (time_t)-1)
    {
      return -1;
    }

  gmtime_r(&t, &tm);
  return tm.tm_year >= 124 ? 0 : -1;  /* 124 = 2024 */
}

static int ui_test_i2c(void)
{
  /* I2C0 is used by the GT911 touch (may be absent) - just check the
   * bus device exists.
   */

  return access("/dev/i2c0", F_OK);
}

static int ui_test_spi(void)
{
  /* SPI0 is the NV3007 panel - if the fb device exists, SPI works. */

  return access("/dev/fb0", F_OK);
}

static const struct ui_term_entry_s g_term_entries[] =
{
  /* NSH commands */

  { "free",        UI_TERM_CMD,  "free",                    NULL },
  { "ps",          UI_TERM_CMD,  "ps",                      NULL },
  { "mount",       UI_TERM_CMD,  "mount",                   NULL },
  { "ifconfig",    UI_TERM_CMD,  "ifconfig",                NULL },
  { "ping gw",     UI_TERM_CMD,  "ping -c 3 192.168.10.1",  NULL },
  { "cpuload",     UI_TERM_CMD,  "cpuload",                 NULL },
  { "ls /data",    UI_TERM_CMD,  "ls /data",                NULL },
  { "date",        UI_TERM_CMD,  "date",                    NULL },

  /* Hardware tests */

  { "test: SD card",   UI_TERM_TEST, NULL, ui_test_sd    },
  { "test: USB disk",  UI_TERM_TEST, NULL, ui_test_usb   },
  { "test: Network",   UI_TERM_TEST, NULL, ui_test_net   },
  { "test: M0 rpmsg",  UI_TERM_TEST, NULL, ui_test_rpmsg },
  { "test: SPI NAND",  UI_TERM_TEST, NULL, ui_test_nand  },
  { "test: RTC/NTP",   UI_TERM_TEST, NULL, ui_test_rtc   },
  { "test: I2C bus",   UI_TERM_TEST, NULL, ui_test_i2c   },
  { "test: SPI panel", UI_TERM_TEST, NULL, ui_test_spi   },
};

static void ui_term_run_cb(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  char buf[64];

  if (idx < 0 || idx >= (int)(sizeof(g_term_entries) /
                             sizeof(g_term_entries[0])))
    {
      return;
    }

  const struct ui_term_entry_s *ent = &g_term_entries[idx];

  if (ent->type == UI_TERM_CMD)
    {
      int ret = system(ent->cmd);

      snprintf(buf, sizeof(buf), "%s -> exit=%d", ent->label, ret);
    }
  else
    {
      int ret = ent->test();

      snprintf(buf, sizeof(buf), "%s -> %s", ent->label,
               ret == 0 ? "PASS" : "FAIL");
    }

  lv_label_set_text(g_cmd_result, buf);
  lv_obj_set_style_text_color(g_cmd_result,
                              lv_color_hex(ent->type == UI_TERM_CMD ||
                                           strstr(buf, "PASS") != NULL ?
                                           UI_COL_ACCENT1 : 0xff6060),
                              0);
}

static void ui_build_cmd(lv_obj_t *page)
{
  lv_obj_t *cont;
  unsigned int i;

  /* Scrollable container for the command + test buttons. */

  cont = lv_obj_create(page);
  lv_obj_set_size(cont, UI_CONTENT_W, 96);
  lv_obj_set_pos(cont, 0, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_pad_all(cont, 2, 0);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(cont, LV_DIR_VER);

  for (i = 0; i < sizeof(g_term_entries) / sizeof(g_term_entries[0]);
       i++)
    {
      lv_obj_t *btn = lv_button_create(cont);
      lv_obj_t *lbl;

      lv_obj_set_size(btn, UI_CONTENT_W - 12, 26);
      ui_style_button(btn);

      /* Test entries get a slightly different accent so they stand out
       * from plain commands.
       */

      if (g_term_entries[i].type == UI_TERM_TEST)
        {
          lv_obj_set_style_border_color(btn, lv_color_hex(UI_COL_ACCENT2),
                                        0);
          lv_obj_set_style_border_width(btn, 1, 0);
        }

      lv_obj_add_event_cb(btn, ui_term_run_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      lbl = lv_label_create(btn);
      lv_label_set_text(lbl, g_term_entries[i].label);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
      lv_obj_center(lbl);
      lv_group_add_obj(lv_group_get_default(), btn);
    }

  /* Result line at the bottom. */

  g_cmd_result = ui_make_label(page, "", &lv_font_montserrat_12,
                               UI_COL_DIM);
  lv_obj_set_pos(g_cmd_result, 0, 100);
}

static void ui_build_info(lv_obj_t *page)
{
  lv_obj_t *lbl = ui_make_label(page,
                                "HD-RK3506-EVM  openvela/NuttX\n"
                                "RK3506G2  3xCortex-A7 + M0\n"
                                "DDR3 128MB  SPI-NAND 256MB\n"
                                "Panel NV3007 142x428 SPI0\n"
                                "Keys: J9-18 NEXT  J9-20 OK",
                                &lv_font_montserrat_12, UI_COL_TEXT);

  lv_obj_set_pos(lbl, 0, 0);

  g_info_uptime = ui_make_label(page, "", &lv_font_montserrat_12,
                                UI_COL_DIM);
  lv_obj_set_pos(g_info_uptime, 0, 100);
}

/****************************************************************************
 * Apps page: list installed quick apps, Run / Del per row.  The list is
 * rebuilt every time the page is shown (ui_apps_fill from ui_show_page).
 * Buttons carry the app index in user data; the path/name is looked up
 * from the snapshot taken at fill time so a stale row can never run the
 * wrong file.
 ****************************************************************************/

static struct qapp_info_s g_apps[QAPP_MAX];
static int                g_apps_count;

static void ui_apps_run_cb(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);

  if (idx >= 0 && idx < g_apps_count)
    {
      qapp_run(g_apps[idx].path);
    }
}

static void ui_apps_del_cb(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);

  if (idx >= 0 && idx < g_apps_count && !g_apps[idx].builtin)
    {
      qapp_delete(g_apps[idx].name);
      ui_apps_fill();
    }
}

static void ui_apps_fill(void)
{
  int i;

  lv_obj_clean(g_apps_list);
  g_apps_count = qapp_list(g_apps, QAPP_MAX);

  if (g_apps_count == 0)
    {
      lv_obj_t *lbl = ui_make_label(g_apps_list,
                                    "no apps - adb push / qapp install",
                                    &lv_font_montserrat_12, UI_COL_DIM);
      lv_obj_set_pos(lbl, 0, 0);
      return;
    }

  for (i = 0; i < g_apps_count; i++)
    {
      lv_obj_t *name;
      lv_obj_t *btn;
      lv_obj_t *lbl;
      int y = i * 30;

      name = ui_make_label(g_apps_list, g_apps[i].name,
                           &lv_font_montserrat_12,
                           g_apps[i].builtin ? UI_COL_DIM : UI_COL_TEXT);
      lv_obj_set_pos(name, 0, y + 6);

      btn = lv_button_create(g_apps_list);
      lv_obj_set_size(btn, 44, 26);
      lv_obj_set_pos(btn, UI_CONTENT_W - 96, y);
      ui_style_button(btn);
      lv_obj_add_event_cb(btn, ui_apps_run_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      lbl = lv_label_create(btn);
      lv_label_set_text(lbl, LV_SYMBOL_PLAY);
      lv_obj_center(lbl);
      lv_group_add_obj(lv_group_get_default(), btn);

      if (!g_apps[i].builtin)
        {
          btn = lv_button_create(g_apps_list);
          lv_obj_set_size(btn, 44, 26);
          lv_obj_set_pos(btn, UI_CONTENT_W - 48, y);
          ui_style_button(btn);
          lv_obj_add_event_cb(btn, ui_apps_del_cb, LV_EVENT_CLICKED,
                              (void *)(intptr_t)i);
          lbl = lv_label_create(btn);
          lv_label_set_text(lbl, LV_SYMBOL_TRASH);
          lv_obj_center(lbl);
          lv_group_add_obj(lv_group_get_default(), btn);
        }
    }
}

static void ui_build_apps(lv_obj_t *page)
{
  lv_obj_t *title = ui_make_label(page, "QuickJS apps",
                                  &lv_font_montserrat_12, UI_COL_DIM);
  lv_obj_set_pos(title, 0, 0);

  g_apps_list = lv_obj_create(page);
  lv_obj_set_size(g_apps_list, UI_CONTENT_W, UI_SCR_H - 30);
  lv_obj_set_pos(g_apps_list, 0, 18);
  lv_obj_set_style_bg_opa(g_apps_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_apps_list, 0, 0);
  lv_obj_set_style_pad_all(g_apps_list, 0, 0);
  lv_obj_set_scroll_dir(g_apps_list, LV_DIR_VER);
}

/****************************************************************************
 * Name: hd_ui_main
 ****************************************************************************/

int hd_ui_main(int argc, char *argv[])
{
  static const char * const menu_names[UI_NPAGES] =
    {
      LV_SYMBOL_HOME,
      LV_SYMBOL_KEYBOARD,
      LV_SYMBOL_PLAY,
      LV_SYMBOL_LIST
    };

  lv_obj_t *scr;
  lv_obj_t *menu;
  int i;

  (void)argc;
  (void)argv;

  scr = lv_screen_active();
  ui_style_screen(scr);

  /* Right-side icon menu FIRST: LVGL group focus order follows the
   * lv_group_add_obj() order, and the J9 NEXT key walks it, so the
   * four menu buttons must be the first members or focus would land
   * on page widgets (textarea/keyboard) the user cannot see yet.
   */

  menu = lv_obj_create(scr);
  lv_obj_set_size(menu, UI_MENU_W, UI_SCR_H - 8);
  lv_obj_set_pos(menu, UI_SCR_W - UI_MENU_W - 3, 0);
  lv_obj_set_style_bg_color(menu, lv_color_hex(UI_COL_MENU_BG), 0);
  lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(menu, 6, 0);
  lv_obj_set_style_border_width(menu, 0, 0);
  lv_obj_set_style_pad_all(menu, 2, 0);
  lv_obj_remove_flag(menu, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(menu, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (i = 0; i < UI_NPAGES; i++)
    {
      lv_obj_t *btn = lv_button_create(menu);
      lv_obj_t *lbl;

      lv_obj_set_size(btn, UI_MENU_W - 6, 30);
      ui_style_button(btn);
      lv_obj_add_event_cb(btn, ui_menu_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      lbl = lv_label_create(btn);
      lv_label_set_text(lbl, menu_names[i]);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
      lv_obj_center(lbl);
      lv_group_add_obj(lv_group_get_default(), btn);
    }

  /* Content pages on the left */

  for (i = 0; i < UI_NPAGES; i++)
    {
      g_pages[i] = lv_obj_create(scr);
      lv_obj_set_size(g_pages[i], UI_CONTENT_W, UI_SCR_H - 8);
      lv_obj_set_pos(g_pages[i], 0, 0);
      ui_style_screen(g_pages[i]);
      lv_obj_set_style_radius(g_pages[i], 0, 0);
      lv_obj_remove_flag(g_pages[i], LV_OBJ_FLAG_SCROLLABLE);
    }

  ui_build_home(g_pages[UI_PAGE_HOME]);
  ui_build_cmd(g_pages[UI_PAGE_CMD]);
  ui_build_info(g_pages[UI_PAGE_INFO]);
  ui_build_apps(g_pages[UI_PAGE_APPS]);

  ui_show_page(UI_PAGE_HOME);

  /* Park the focus ring on the HOME menu button at startup. */

  {
    lv_group_t *group = lv_group_get_default();
    lv_obj_t *first = lv_obj_get_child(menu, 0);

    if (group != NULL && first != NULL)
      {
        lv_group_focus_obj(first);
      }
  }

  lv_timer_create(ui_refresh_cb, UI_REFRESH_MS, NULL);

  return 0;
}

#endif /* CONFIG_GRAPHICS_LVGL && CONFIG_HD_RK3506_UI */
