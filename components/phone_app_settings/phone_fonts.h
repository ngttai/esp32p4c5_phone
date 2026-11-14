#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(phone_font_18)
LV_FONT_DECLARE(phone_font_20)
LV_FONT_DECLARE(phone_font_24)
LV_FONT_DECLARE(phone_font_28)

/** Vietnamese-capable UI font closest to size_px (18, 20, 24 or 28 px). */
const lv_font_t *phone_font_by_size(int size_px);

#ifdef __cplusplus
}
#endif
