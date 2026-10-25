/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_lv_port_indev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two external push buttons on J9, wired to the LVGL default group as a
 * keypad input device:
 *
 *   "next"  J9-33 = GPIO1_D1  (VO_LCDC_D2 position; RGB LCD not used)
 *           LV_KEY_NEXT  - move focus to the next widget
 *   "ok"    J9-32 = GPIO1_D2  (VO_LCDC_D1 position; RGB LCD not used)
 *           LV_KEY_ENTER - activate the focused widget
 *
 * Both pins are active low (button shorts the pad to GND) with the SoC
 * internal pull-up enabled, so no external pull resistor is required.
 * Levels are debounced for 20 ms inside the LVGL indev read callback,
 * which runs from the existing lv_timer task (~30 Hz): no IRQ or extra
 * thread is involved.
 *
 * Both pads are plain non-RM_IO GPIOs.  The RM_IO pads on J9 (CAN/UART
 * positions, GPIO0_C2/C4 on J9-18/20) were tried first and read back
 * stuck regardless of the pad level - the RM_IO matrix does not pass
 * the pad through to the GPIO block's EXT_PORT - so the buttons live
 * on bank 1 D-group pins instead.  Pin numbering follows the HD board
 * doc formula: global GPIO = bank*32 + linear pin.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <debug.h>

#include <lvgl/lvgl.h>

#include "drv/gpio/rk3506_gpio.h"

#if defined(CONFIG_GRAPHICS_LVGL) && defined(CONFIG_HD_RK3506_LV_BUTTONS)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The Kconfig values are already global GPIO numbers (bank*32 + pin),
 * which is exactly the packed RK3506_GPIO_PIN() encoding.
 */

#ifndef CONFIG_HD_RK3506_BTN_NEXT_PIN
#  define CONFIG_HD_RK3506_BTN_NEXT_PIN 57   /* GPIO1_D1 -> J9-33 */
#endif

#ifndef CONFIG_HD_RK3506_BTN_OK_PIN
#  define CONFIG_HD_RK3506_BTN_OK_PIN   58   /* GPIO1_D2 -> J9-32 */
#endif

#define BTN_NEXT_GPIN            ((uint8_t)CONFIG_HD_RK3506_BTN_NEXT_PIN)
#define BTN_OK_GPIN              ((uint8_t)CONFIG_HD_RK3506_BTN_OK_PIN)

/* Active low: the pressed level on the pad is 0. */

#define BTN_PRESSED_LEVEL        false

/* Raw input must be stable for this long before it is accepted. */

#define BTN_DEBOUNCE_MS          20

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3506_btn_s
{
  uint8_t  gpin;           /* Packed RK3506 GPIO id                    */
  uint32_t key;            /* LVGL key code this button produces       */
  bool     pressed;        /* Debounced state: true while pressed      */
  bool     raw;            /* Last raw (active-true) sample            */
  uint32_t change_ms;      /* lv_tick_get() at the first raw change    */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3506_btn_s g_btn[2];

/* Key + state that still needs its LVGL RELEASED report. */

static uint32_t g_release_key;
static bool     g_release_pending;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool btn_raw_pressed(const struct rk3506_btn_s *btn)
{
  bool level = !BTN_PRESSED_LEVEL;

  if (rk3506_gpio_read(btn->gpin, &level) < 0)
    {
      return false;
    }

  return level == BTN_PRESSED_LEVEL;
}

static void btn_debounce(struct rk3506_btn_s *btn)
{
  bool now = btn_raw_pressed(btn);

  if (now != btn->raw)
    {
      btn->raw       = now;
      btn->change_ms = lv_tick_get();
    }

  if (now != btn->pressed &&
      (uint32_t)(lv_tick_get() - btn->change_ms) >= BTN_DEBOUNCE_MS)
    {
      btn->pressed = now;
    }
}

static void rk3506_btn_read(lv_indev_t *indev, lv_indev_data_t *data)
{
  unsigned int i;

  for (i = 0; i < sizeof(g_btn) / sizeof(g_btn[0]); i++)
    {
      btn_debounce(&g_btn[i]);
    }

  /* One key at a time; report the first button that is held. */

  for (i = 0; i < sizeof(g_btn) / sizeof(g_btn[0]); i++)
    {
      if (g_btn[i].pressed)
        {
          /* Keep reporting PRESSED while held; the matching RELEASED is
           * sent on the first scan after release.
           */

          g_release_pending = true;
          g_release_key     = g_btn[i].key;
          data->state       = LV_INDEV_STATE_PRESSED;
          data->key         = g_btn[i].key;
          return;
        }
    }

  /* No button held: send exactly one RELEASED for the last key so LVGL
   * closes the click/long-press state machine.
   */

  if (g_release_pending)
    {
      data->state = LV_INDEV_STATE_RELEASED;
      data->key   = g_release_key;
      g_release_pending = false;
      return;
    }

  /* Idle: keep reporting released with no meaningful key. */

  data->state = LV_INDEV_STATE_RELEASED;
  data->key   = 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_lv_port_indev_init
 *
 * Description:
 *   Configure the two J9 button pins and register an LVGL keypad input
 *   device bound to the LVGL default group.  Must run after the display is
 *   created (hd_rk3506_lv_port_disp_init()).
 *
 ****************************************************************************/

int hd_rk3506_lv_port_indev_init(void)
{
  lv_indev_t *indev;
  lv_group_t *group;

  g_btn[0].gpin = BTN_NEXT_GPIN;
  g_btn[0].key  = LV_KEY_NEXT;
  g_btn[1].gpin = BTN_OK_GPIN;
  g_btn[1].key  = LV_KEY_ENTER;

  if (rk3506_gpio_config(BTN_NEXT_GPIN,
                         RK3506_GPIO_INPUT | RK3506_GPIO_PULL_UP) < 0)
    {
      _err("BTN: GPIO1_D1 (J9-33) input config failed\n");
      return -1;
    }

  if (rk3506_gpio_config(BTN_OK_GPIN,
                         RK3506_GPIO_INPUT | RK3506_GPIO_PULL_UP) < 0)
    {
      _err("BTN: GPIO1_D2 (J9-32) input config failed\n");
      return -1;
    }

  /* Make the default group focusable widgets land in when the UI creates
   * them, and point the keypad at it.
   */

  group = lv_group_create();
  if (group == NULL)
    {
      _err("BTN: lv_group_create failed\n");
      return -1;
    }

  lv_group_set_default(group);

  indev = lv_indev_create();
  if (indev == NULL)
    {
      _err("BTN: lv_indev_create failed\n");
      return -1;
    }

  lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(indev, rk3506_btn_read);
  lv_indev_set_group(indev, group);

  _info("BTN: keypad indev registered (next=GPIO1_D1/J9-33, "
        "ok=GPIO1_D2/J9-32, active low)\n");
  return 0;
}

#endif /* CONFIG_GRAPHICS_LVGL && CONFIG_HD_RK3506_LV_BUTTONS */
