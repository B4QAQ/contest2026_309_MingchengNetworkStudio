/****************************************************************************
 * vendor/rockchip/boards/rk3506/hd-rk3506-evm/src/hd_rk3506_lv_port_indev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Four external push buttons on J9:
 *
 *   UP     J9-33 = GPIO1_D1 (VO_LCDC_D2 position)  -> LV_KEY_PREV (focus prev)
 *   DOWN   J9-32 = GPIO1_D2 (VO_LCDC_D1 position)  -> LV_KEY_NEXT (focus next)
 *   OK     J9-34 = GPIO1_D0 (VO_LCDC_D3 position)  -> LV_KEY_ENTER (click)
 *   ESC    J9-35 = GPIO1_C7 (VO_LCDC_D4 position)  -> back, see below
 *
 * UP/DOWN/OK go through the LVGL keypad driver: it moves focus on
 * NEXT/PREV and sends ENTER to the focused object (PRESSED/CLICKED).
 * No short/long press discrimination - each button emits exactly one
 * key on a simple press.
 *
 * ESC is deliberately NOT sent into LVGL.  The keypad driver delivers
 * ESC with lv_group_send_data(), which hands LV_EVENT_KEY to the
 * *focused object* only (core/lv_group.c:305) and that event bubbles
 * (core/lv_obj_event.c:393) only when the object carries
 * LV_OBJ_FLAG_EVENT_BUBBLE, which is off by default.  So an ESC handler
 * installed on the screen stops firing the moment a focusable child
 * (any widget an app creates) takes group focus - exactly the
 * "back key stops working inside a quick app" symptom.
 *
 * Instead ESC travels through hd_rk3506_back_register() handlers, which
 * are independent of group focus.  It is dispatched from lv_async_call()
 * so the handler may freely delete the active screen, which is unsafe
 * from inside indev_read() / an object event dispatch.
 *
 * All pins are active low (button shorts the pad to GND) with the SoC
 * internal pull-up enabled, so no external pull resistor is required.
 * Levels are debounced for 20 ms inside the LVGL indev read callback,
 * which runs from the existing lv_timer task (~30 Hz): no IRQ or extra
 * thread is involved.
 *
 * All five pads are plain non-RM_IO GPIOs.  Pin numbering follows the HD
 * board doc formula: global GPIO = bank*32 + linear pin.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include <debug.h>

#include <lvgl/lvgl.h>

#include "drv/gpio/rk3506_gpio.h"

#include "hd_rk3506.h"

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

#ifndef CONFIG_HD_RK3506_BTN_ESC_PIN
#  define CONFIG_HD_RK3506_BTN_ESC_PIN   55   /* GPIO1_C7 -> J9-35 */
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
  uint32_t          key;         /* LVGL key, 0 = back button         */
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
  { CONFIG_HD_RK3506_BTN_UP_PIN,   LV_KEY_PREV,  S_IDLE, false, false, 0 },
  { CONFIG_HD_RK3506_BTN_DOWN_PIN, LV_KEY_NEXT,  S_IDLE, false, false, 0 },
  { CONFIG_HD_RK3506_BTN_OK_PIN,   LV_KEY_ENTER, S_IDLE, false, false, 0 },
  { CONFIG_HD_RK3506_BTN_ESC_PIN,  0,            S_IDLE, false, false, 0 },
};

/* Back-key handler stack.  The last registration wins, so a running
 * quick app shadows the UI's default "return to home" handler and
 * simply unregisters itself when it stops.
 */

#define BTN_BACK_MAX_HANDLERS    4

static hd_rk3506_back_handler_t g_back_cb[BTN_BACK_MAX_HANDLERS];
static FAR void                *g_back_arg[BTN_BACK_MAX_HANDLERS];

/* Set while a back action is queued, so holding ESC fires it once. */

static bool g_back_pending;

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

/* Runs from the LVGL timer task outside any event dispatch, where a
 * handler is free to delete the active screen.
 */

static void btn_back_async(FAR void *arg)
{
  hd_rk3506_back_handler_t cb = NULL;
  FAR void *cbarg = NULL;
  int i;

  (void)arg;

  g_back_pending = false;

  for (i = BTN_BACK_MAX_HANDLERS - 1; i >= 0; i--)
    {
      if (g_back_cb[i] != NULL)
        {
          cb    = g_back_cb[i];
          cbarg = g_back_arg[i];
          break;
        }
    }

  if (cb != NULL)
    {
      cb(cbarg);
    }
}

static void btn_back_fire(void)
{
  if (g_back_pending)
    {
      return;
    }

  g_back_pending = true;
  lv_async_call(btn_back_async, NULL);
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
        btn->state = S_UP;

        if (btn->key == 0)
          {
            /* Back button: bypass LVGL entirely. */

            btn_back_fire();
            break;
          }

        *key   = btn->key;
        *state = LV_INDEV_STATE_PRESSED;
        return true;

      case S_UP:
        if (!btn->pressed)
          {
            btn->state = S_IDLE;

            if (btn->key == 0)
              {
                break;
              }

            *key   = btn->key;
            *state = LV_INDEV_STATE_RELEASED;
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

  /* Idle: keep reporting released with no meaningful key.  LVGL's
   * keypad driver substitutes prev_key on release, so a RELEASED with
   * key=0 still completes the click for the previously pressed key.
   */

  data->state = LV_INDEV_STATE_RELEASED;
  data->key   = 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hd_rk3506_back_register
 *
 * Description:
 *   Push a handler for the ESC button.  The most recently registered
 *   handler runs first and the earlier ones stay as fallbacks, so a
 *   quick app can take the back key over while it runs and hand it back
 *   by calling hd_rk3506_back_unregister() when it stops.  Handlers run
 *   from the LVGL timer task, outside any event dispatch.
 *
 * Returned Value:
 *   OK, or -ENOMEM when every slot is taken.
 *
 ****************************************************************************/

int hd_rk3506_back_register(hd_rk3506_back_handler_t cb, FAR void *arg)
{
  int i;

  if (cb == NULL)
    {
      return -EINVAL;
    }

  for (i = 0; i < BTN_BACK_MAX_HANDLERS; i++)
    {
      if (g_back_cb[i] == NULL)
        {
          g_back_cb[i]  = cb;
          g_back_arg[i] = arg;
          return OK;
        }
    }

  return -ENOMEM;
}

/****************************************************************************
 * Name: hd_rk3506_back_unregister
 *
 * Description:
 *   Remove a handler installed by hd_rk3506_back_register().
 *
 ****************************************************************************/

void hd_rk3506_back_unregister(hd_rk3506_back_handler_t cb)
{
  int i;

  for (i = 0; i < BTN_BACK_MAX_HANDLERS; i++)
    {
      if (g_back_cb[i] == cb)
        {
          g_back_cb[i]  = NULL;
          g_back_arg[i] = NULL;
        }
    }
}

/****************************************************************************
 * Name: hd_rk3506_lv_port_indev_init
 *
 * Description:
 *   Configure the four J9 button pins and register an LVGL keypad input
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

  _info("BTN: keypad indev + back key registered "
        "(UP=J9-33 DOWN=J9-32 OK=J9-34 ESC=J9-35 back)\n");
  return 0;
}

#endif /* CONFIG_GRAPHICS_LVGL && CONFIG_HD_RK3506_LV_BUTTONS */
