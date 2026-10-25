/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_lv_port_indev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two external push buttons on J9, wired to the LVGL default group as an
 * encoder input device with short/long press discrimination:
 *
 *   button 1  J9-33 = GPIO1_D1 (VO_LCDC_D2 position; RGB LCD not used)
 *             short = LV_KEY_LEFT  (prev / left)  long = LV_KEY_ENTER (OK)
 *   button 2  J9-32 = GPIO1_D2 (VO_LCDC_D1 position; RGB LCD not used)
 *             short = LV_KEY_RIGHT (next / right) long = LV_KEY_ESC   (BACK)
 *
 * Registered as LV_INDEV_TYPE_ENCODER: LVGL's encoder driver turns
 * LV_KEY_LEFT/RIGHT into encoder steps, which move focus in navigate
 * mode and send LV_KEY_LEFT/RIGHT to the focused object in edit mode -
 * exactly what a keyboard (buttonmatrix) needs.  LVGL's keypad driver
 * unconditionally exits edit mode on NEXT/PREV, so it cannot drive a
 * keyboard with only two buttons.
 *
 * Both pins are active low (button shorts the pad to GND) with the SoC
 * internal pull-up enabled, so no external pull resistor is required.
 * Levels are debounced for 20 ms inside the LVGL indev read callback,
 * which runs from the existing lv_timer task (~30 Hz): no IRQ or extra
 * thread is involved.
 *
 * Short vs long: a press released before 500 ms reports the short key
 * on release (PRESSED and RELEASED on two consecutive polls - LVGL
 * needs both edges); reaching 500 ms while still held reports the long
 * key immediately, with RELEASED when the finger lifts.  LVGL's keypad
 * handler takes PREV/NEXT/ENTER/ESC on the PRESSED edge and completes
 * ENTER's click on the matching RELEASED.
 *
 * Both pads are plain non-RM_IO GPIOs.  Pin numbering follows the HD
 * board doc formula: global GPIO = bank*32 + linear pin.
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

#define BTN1_GPIN                ((uint8_t)CONFIG_HD_RK3506_BTN_NEXT_PIN)
#define BTN2_GPIN                ((uint8_t)CONFIG_HD_RK3506_BTN_OK_PIN)

/* Active low: the pressed level on the pad is 0. */

#define BTN_PRESSED_LEVEL        false

/* Raw input must be stable for this long before it is accepted. */

#define BTN_DEBOUNCE_MS          20

/* Held this long -> long press. */

#define BTN_LONG_MS              500

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Per-button short/long state machine.  LVGL consumes at most one key
 * event per poll, so each state emits at most one report and a button
 * that loses arbitration simply keeps its pending state for the next
 * poll.
 */

enum btn_state_e
{
  S_IDLE,            /* not pressed, nothing pending                  */
  S_DOWN,            /* held, deciding short vs long                  */
  S_SHORT_PRESS,     /* released quickly: emit short PRESSED          */
  S_SHORT_RELEASE,   /* emit short RELEASED, then idle                */
  S_LONG_PRESS,      /* long key emitted as PRESSED, still held       */
  S_LONG_RELEASE     /* emit long RELEASED on release, then idle      */
};

struct rk3506_btn_s
{
  uint8_t           gpin;        /* Packed RK3506 GPIO id             */
  uint32_t          short_key;   /* LVGL key on a short press         */
  uint32_t          long_key;    /* LVGL key on a long press          */
  enum btn_state_e  state;
  bool              pressed;     /* Debounced level                   */
  bool              raw;         /* Last raw (active-true) sample     */
  uint32_t          change_ms;   /* lv_tick_get() at first raw change */
  uint32_t          down_ms;     /* lv_tick_get() at the press edge   */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3506_btn_s g_btn[2];

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

/* Advance one button's FSM; returns true when a report is ready. */

static bool btn_poll(struct rk3506_btn_s *btn, uint32_t *key,
                     lv_indev_state_t *state)
{
  uint32_t now = lv_tick_get();

  switch (btn->state)
    {
      case S_IDLE:
        if (btn->pressed)
          {
            btn->down_ms = now;
            btn->state   = S_DOWN;
          }
        break;

      case S_DOWN:
        if (!btn->pressed)
          {
            /* Short press: report PRESSED now, RELEASED next poll. */

            *key       = btn->short_key;
            *state     = LV_INDEV_STATE_PRESSED;
            btn->state = S_SHORT_RELEASE;
            return true;
          }

        if ((uint32_t)(now - btn->down_ms) >= BTN_LONG_MS)
          {
            /* Long press: report the long key immediately. */

            *key       = btn->long_key;
            *state     = LV_INDEV_STATE_PRESSED;
            btn->state = S_LONG_RELEASE;
            return true;
          }
        break;

      case S_SHORT_RELEASE:
        *key       = btn->short_key;
        *state     = LV_INDEV_STATE_RELEASED;
        btn->state = S_IDLE;
        return true;

      case S_LONG_RELEASE:
        if (!btn->pressed)
          {
            *key       = btn->long_key;
            *state     = LV_INDEV_STATE_RELEASED;
            btn->state = S_IDLE;
            return true;
          }
        break;

      default:
        btn->state = S_IDLE;
        break;
    }

  return false;
}

static void rk3506_btn_read(lv_indev_t *indev, lv_indev_data_t *data)
{
  unsigned int i;

  (void)indev;

  for (i = 0; i < sizeof(g_btn) / sizeof(g_btn[0]); i++)
    {
      btn_debounce(&g_btn[i]);
    }

  for (i = 0; i < sizeof(g_btn) / sizeof(g_btn[0]); i++)
    {
      uint32_t         key;
      lv_indev_state_t state;

      if (btn_poll(&g_btn[i], &key, &state))
        {
          data->key   = key;
          data->state = state;
          return;
        }
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

  g_btn[0].gpin      = BTN1_GPIN;
  g_btn[0].short_key = LV_KEY_LEFT;    /* UP / prev in nav, left in edit  */
  g_btn[0].long_key  = LV_KEY_ENTER;   /* OK   */
  g_btn[1].gpin      = BTN2_GPIN;
  g_btn[1].short_key = LV_KEY_RIGHT;   /* DOWN / next in nav, right in edit*/
  g_btn[1].long_key  = LV_KEY_ESC;     /* BACK */

  if (rk3506_gpio_config(BTN1_GPIN,
                         RK3506_GPIO_INPUT | RK3506_GPIO_PULL_UP) < 0)
    {
      _err("BTN: GPIO1_D1 (J9-33) input config failed\n");
      return -1;
    }

  if (rk3506_gpio_config(BTN2_GPIN,
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

  lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
  lv_indev_set_read_cb(indev, rk3506_btn_read);
  lv_indev_set_group(indev, group);

  _info("BTN: encoder indev registered "
        "(btn1=J9-33 short LEFT long OK, btn2=J9-32 short RIGHT long BACK)\n");
  return 0;
}

#endif /* CONFIG_GRAPHICS_LVGL && CONFIG_HD_RK3506_LV_BUTTONS */
