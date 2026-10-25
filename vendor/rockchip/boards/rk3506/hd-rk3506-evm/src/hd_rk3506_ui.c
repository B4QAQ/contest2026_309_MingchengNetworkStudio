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
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <lvgl/lvgl.h>

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
#define UI_PAGE_TEST    2
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
static lv_obj_t *g_cmd_ta;
static lv_obj_t *g_cmd_result;
static lv_obj_t *g_test_lbl[4];

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

static void ui_fmt_uptime(char *buf, size_t len)
{
  struct timespec ts;
  unsigned long s;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  s = (unsigned long)ts.tv_sec;
  snprintf(buf, len, "up %lu:%02lu:%02lu",
           s / 3600, (s / 60) % 60, s % 60);
}

static void ui_fmt_mem(char *buf, size_t len)
{
  struct mallinfo mi = mallinfo();

  snprintf(buf, len, "%uK / %uK",
           (unsigned)(mi.uordblks / 1024),
           (unsigned)((mi.uordblks + mi.fordblks) / 1024));
}

static void ui_fmt_net(char *buf, size_t len)
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

  ui_fmt_uptime(buf, sizeof(buf));
  lv_label_set_text(g_card_lbl[0], buf);

  ui_fmt_mem(buf, sizeof(buf));
  lv_label_set_text(g_card_lbl[1], buf);

  ui_fmt_net(buf, sizeof(buf));
  lv_label_set_text(g_card_lbl[2], buf);

  ui_fmt_usb(buf, sizeof(buf));
  lv_label_set_text(g_card_lbl[3], buf);
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

  lbl = ui_make_label(card, "-", &lv_font_montserrat_14,
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

static void ui_cmd_run_cb(lv_event_t *e)
{
  const char *cmd = lv_textarea_get_text(g_cmd_ta);
  char buf[48];
  int ret;

  (void)e;

  if (cmd == NULL || cmd[0] == '\0')
    {
      return;
    }

  ret = system(cmd);
  snprintf(buf, sizeof(buf), "exit=%d", ret);
  lv_label_set_text(g_cmd_result, buf);
}

static void ui_build_cmd(lv_obj_t *page)
{
  lv_obj_t *kb;
  lv_obj_t *btn;
  lv_obj_t *lbl;

  g_cmd_ta = lv_textarea_create(page);
  lv_obj_set_size(g_cmd_ta, UI_CONTENT_W - 64, 30);
  lv_obj_set_pos(g_cmd_ta, 0, 0);
  lv_textarea_set_one_line(g_cmd_ta, true);
  lv_textarea_set_placeholder_text(g_cmd_ta, "nsh cmd...");
  lv_obj_set_style_bg_color(g_cmd_ta, lv_color_hex(UI_COL_CARD), 0);
  lv_obj_set_style_text_color(g_cmd_ta, lv_color_hex(UI_COL_TEXT), 0);
  lv_obj_set_style_radius(g_cmd_ta, 6, 0);
  lv_obj_set_style_border_width(g_cmd_ta, 1, 0);
  lv_obj_set_style_border_color(g_cmd_ta, lv_color_hex(UI_COL_CARD_BR),
                                0);
  lv_group_add_obj(lv_group_get_default(), g_cmd_ta);

  btn = lv_button_create(page);
  lv_obj_set_size(btn, 56, 30);
  lv_obj_set_pos(btn, UI_CONTENT_W - 58, 0);
  ui_style_button(btn);
  lv_obj_add_event_cb(btn, ui_cmd_run_cb, LV_EVENT_CLICKED, NULL);
  lbl = lv_label_create(btn);
  lv_label_set_text(lbl, "Run");
  lv_obj_center(lbl);
  lv_group_add_obj(lv_group_get_default(), btn);

  g_cmd_result = ui_make_label(page, "", &lv_font_montserrat_12,
                               UI_COL_DIM);
  lv_obj_set_pos(g_cmd_result, 0, 32);

  kb = lv_keyboard_create(page);
  lv_obj_set_size(kb, UI_CONTENT_W, 96);
  lv_obj_set_pos(kb, 0, 40);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_12, 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COL_BG), 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COL_BTN),
                            LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, lv_color_hex(UI_COL_TEXT),
                              LV_PART_ITEMS);
  lv_keyboard_set_textarea(kb, g_cmd_ta);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_group_add_obj(lv_group_get_default(), kb);
}

static void ui_test_run_cb(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  const char *result = "FAIL";

  switch (idx)
    {
      case 0:   /* SD card */
        if (access("/dev/mmcsd0", F_OK) == 0)
          {
            result = "PASS";
          }
        break;

      case 1:   /* USB disk */
        if (access("/dev/sda", F_OK) == 0)
          {
            result = "PASS";
          }
        break;

      case 2:   /* network: eth0 has an IP */
          {
            char buf[48];

            ui_fmt_net(buf, sizeof(buf));
            if (strcmp(buf, "no link") != 0)
              {
                result = "PASS";
              }
          }
        break;

      case 3:   /* M0 rpmsg link */
        if (access("/dev/rpmsg/mcu", F_OK) == 0 ||
            access("/dev/rpmsg-rpmsg-mcu0-echo", F_OK) == 0)
          {
            result = "PASS";
          }
        break;

      default:
        break;
    }

  lv_label_set_text(g_test_lbl[idx], result);
  lv_obj_set_style_text_color(g_test_lbl[idx],
                              lv_color_hex(result[0] == 'P' ?
                                           UI_COL_ACCENT1 : 0xff6060),
                              0);
}

static const char * const g_test_names[4] =
{
  "SD card", "USB disk", "Network", "M0 rpmsg"
};

static void ui_build_test(lv_obj_t *page)
{
  lv_obj_t *note;
  int i;

  for (i = 0; i < 4; i++)
    {
      lv_obj_t *btn = lv_button_create(page);
      lv_obj_t *lbl;

      lv_obj_set_size(btn, 96, 30);
      lv_obj_set_pos(btn, (i % 2) * 170, (i / 2) * 40);
      ui_style_button(btn);
      lv_obj_add_event_cb(btn, ui_test_run_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      lbl = lv_label_create(btn);
      lv_label_set_text(lbl, g_test_names[i]);
      lv_obj_center(lbl);
      lv_group_add_obj(lv_group_get_default(), btn);

      g_test_lbl[i] = ui_make_label(page, "-", &lv_font_montserrat_12,
                                    UI_COL_DIM);
      lv_obj_set_pos(g_test_lbl[i], (i % 2) * 170 + 104,
                     (i / 2) * 40 + 8);
    }

  note = ui_make_label(page, "NEXT moves, ENTER runs",
                       &lv_font_montserrat_12, UI_COL_DIM);
  lv_obj_set_pos(note, 0, 84);
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
      LV_SYMBOL_OK,
      LV_SYMBOL_LIST
    };

  lv_obj_t *scr;
  lv_obj_t *menu;
  int i;

  (void)argc;
  (void)argv;

  scr = lv_screen_active();
  ui_style_screen(scr);

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
  ui_build_test(g_pages[UI_PAGE_TEST]);
  ui_build_info(g_pages[UI_PAGE_INFO]);

  /* Right-side icon menu; every button joins the default group so the
   * J9 keys can walk and activate them.
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

  ui_show_page(UI_PAGE_HOME);

  lv_timer_create(ui_refresh_cb, UI_REFRESH_MS, NULL);

  return 0;
}

#endif /* CONFIG_GRAPHICS_LVGL && CONFIG_HD_RK3506_UI */
