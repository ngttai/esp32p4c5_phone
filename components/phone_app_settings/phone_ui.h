#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The scrollbar of the Settings app on a scrolling list: a thin bar on the right, shown only while the
 * list moves (objects made with lv_obj_remove_style_all() have none). Call with the LVGL lock held.
 */
void phone_ui_scrollbar(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
