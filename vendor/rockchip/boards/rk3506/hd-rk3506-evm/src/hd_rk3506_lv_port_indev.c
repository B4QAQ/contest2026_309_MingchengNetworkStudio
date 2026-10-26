/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_lv_port_indev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Four external push buttons on J9, wired to the LVGL default group as an
 * encoder input device:
 *
 *   UP    J9-33 = GPIO1_D1 (VO_LCDC_D2 position)  -> LV_KEY_LEFT
 *   DOWN  J9-32 = GPIO1_D2 (VO_LCDC_D1 position)  -> LV_KEY_RIGHT
 *   OK    J9-34 = GPIO1_D0 (VO_LCDC_D3 position)  -> LV_KEY_ENTER
 *   BACK  J9-35 = GPIO1_C7 (VO_LCDC_D4 position)  -> LV_KEY_ESC
 *
 * Registered as LV_INDEV_TYPE_ENCODER: LVGL's encoder driver turns
 * LV_KEY_LEFT/RIGHT into encoder steps, which move focus in navigate
 * mode and send LV_KEY_LEFT/RIGHT to the focused object in edit mode -
 * exactly what a keyboard (buttonmatrix) needs.  LVGL's keypad driver
 * unconditionally exits edit mode on NEXT/PREV, so it cannot drive a
 * keyboard with only UP/DOWN.
 *
 * All pins are active low (button shorts the pad to GND) with the SoC
 * internal pull-up enabled, so no external pull resistor is required.
 * Levels are debounced for 20 ms inside the LVGL indev read callback,
 * which runs from the existing lv_timer task (~30 Hz): no IRQ or extra
 * thread is involved.
 *
 * All four pads are plain non-RM_IO GPIOs.  Pin numbering follows the HD
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

#ifndef CONFIG_HD_RK3506_BTN_UP_PIN
#  define CONFIG_HD_RK3506_BTN_UP_PIN    57   /* GPIO1_D1 -> J9-33 */
#endif

#ifndef CONFIG_HD_RK3506_BTN_DOWN_PIN
#  define CONFIG_HD_RK3506_BTN_DOWN_PIN  58   /* GPIO1_D2 -> J9-32 */
#endif

#ifndef CONFIG_HD_RK3506_BTN_OK_PIN
#  define CONFIG_HD_RK3506_BTN_OK_PIN    56   /* GPIO1_D0 -> J9-34 */
#endif

#ifndef CONFIG_HD_RK3506_BTN_BACK_PIN
#  define CONFIG_HD_RK3506_BTN_BACK_PIN  55   /* GPIO1_C7 -> J9-35 */
#endif

/* Active low: the pressed level on the pad is 0. */

#define BTN_PRESSED_LEVEL        false

/* Raw input must be stable for this long before it is accepted. */

#define BTN_DEBOUNCE_MS          20

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Per-button state machine.  LVGL consumes at most one key event per
 * poll, so each state emits at most one report and a button that loses
 * arbitration simply keeps its pending state for the next poll.
 */

enum btn_state_e
{
  S_IDLE,            /* not pressed, nothing pending                  */
  S_DOWN,            /* pressed, emit PRESSED next poll               */
  S_UP               /* released, emit RELEASED next poll             */
};

struct rk3506_btn_s
{
  uint8_t           gpin;        /* Packed RK3506 GPIO id             */
  uint32_t          key;         /* LVGL key this button emits        */
  enum btn_state_e  state;
  bool              pressed;     /* Debounced level                   */
  bool              raw;         /* Last raw (active-true) sample     */
  uint32_t          change_ms;   /* lv_tick_get() at first raw change */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3506_btn_s g_btn[] =
{
  { CONFIG_HD_RK3506_BTN_UP_PIN,   LV_KEY_LEFT,  S_IDLE, false, false, 0 },
  { CONFIG_HD_RK3506_BTN_DOWN_PIN, LV_KEY_RIGHT, S_IDLE, false, false, 0 },
  { CONFIG_HD_RK3506_BTN_OK_PIN,   LV_KEY_ENTER, S_IDLE, false, false, 0 },
  { CONFIG_HD_RK3506_BTN_BACK_PIN, LV_KEY_ESC,   S_IDLE, false, false, 0 },
};

/* Key whose PRESSED we last reported and which is still physically held.
 * The read callback must keep reporting PRESSED with this key between
 * FSM events - reporting an idle RELEASED would make LVGL see a bogus
 * press-release ~30 ms after the PRESSED even though the user is still
 * holding the button.
 */

static uint32_t g_held_key = 0;

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
  switch (btn->state)
    {
      case S_IDLE:
        if (btn->pressed)
          {
            btn->state = S_DOWN;
          }
        break;

      case S_DOWN:
        *key       = btn->key;
        *state     = LV_INDEV_STATE_PRESSED;
        btn->state = S_UP;
        return true;

      case S_UP:
        if (!btn->pressed)
          {
            *key       = btn->key;
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
          if (state == LV_INDEV_STATE_PRESSED)
            {
              g_held_key = key;
            }
          else
            {
              g_held_key = 0;
            }

          data->key   = key;
          data->state = state;
          return;
        }
    }

  /* Between FSM events: while a reported key is still physically held
   * we must keep reporting PRESSED with that key.  Reporting an idle
   * RELEASED here would make LVGL see a bogus press-release ~30 ms
   * after the PRESSED even though the user is still holding the button
   * - and the encoder release branch keys off data->key (no prev_key
   * fallback like the keypad branch), so a RELEASED with key=0 silently
   * drops the CLICKED event.  A sustained PRESSED also lets LVGL's own
   * long-press timer run, which is what toggles edit mode on editable
   * widgets (e.g. the keyboard).
   */

  if (g_held_key != 0)
    {
      data->key   = g_held_key;
      data->state = LV_INDEV_STATE_PRESSED;
    }
  else
    {
      data->key   = 0;
      data->state = LV_INDEV_STATE_RELEASED;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_lv_port_indev_init
 *
 * Description:
 *   Configure the four J9 button pins and register an LVGL encoder input
 *   device bound to the LVGL default group.  Must run after the display
 *   is created (hd_rk3506_lv_port_disp_init()).
 *
 ****************************************************************************/

int hd_rk3506_lv_port_indev_init(void)
{
  lv_indev_t *indev;
  lv_group_t *group;
  unsigned int i;

  for (i = 0; i < sizeof(g_btn) / sizeof(g_btn[0]); i++)
    {
      if (rk3506_gpio_config(g_btn[i].gpin,
                             RK3506_GPIO_INPUT | RK3506_GPIO_PULL_UP) < 0)
        {
          _err("BTN: GPIO pin %u input config failed\n",
               (unsigned)g_btn[i].gpin);
          return -1;
        }
    }

  /* Make the default group focusable widgets land in when the UI creates
   * them, and point the encoder at it.
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
        "(UP=J9-33 DOWN=J9-32 OK=J9-34 BACK=J9-35)\n");
  return 0;
}

#endif /* CONFIG_GRAPHICS_LVGL && CONFIG_HD_RK3506_LV_BUTTONS */
