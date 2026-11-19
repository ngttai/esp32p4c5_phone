#include "phone_ui.h"

void phone_ui_scrollbar(lv_obj_t *obj)
{
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(obj, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(obj, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(obj, 8, LV_PART_SCROLLBAR);
}
