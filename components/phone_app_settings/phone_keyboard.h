#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Phone-style on-screen keyboard (dark keys, QWERTY with centered middle row, Shift / Backspace beside the bottom
 * letters, "?123" symbol pages, popover of the pressed letter). Built on lv_keyboard with its own key handler.
 * Sends LV_EVENT_READY (OK key) and LV_EVENT_CANCEL (hide key) on the keyboard object, like lv_keyboard.
 */
lv_obj_t *phone_keyboard_create(lv_obj_t *parent, lv_obj_t *textarea);

/** Back to the lowercase letters (call when the keyboard is shown again). */
void phone_keyboard_reset(lv_obj_t *kb);

#ifdef __cplusplus
}
#endif
