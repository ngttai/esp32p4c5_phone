#include "phone_power.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "phone_fonts.h"
#include "phone_locale.h"

#define TAG "PWR"

namespace {

constexpr uint32_t COLOR_DIM = 0x000000;
constexpr uint32_t COLOR_CARD = 0x1C252E;
constexpr uint32_t COLOR_TEXT = 0xFFFFFF;
constexpr uint32_t COLOR_RESTART = 0x2F80ED;
constexpr uint32_t COLOR_CANCEL = 0x3A4652;

lv_obj_t *s_menu;
lv_obj_t *s_card;

void close_menu()
{
    if (s_menu) {
        lv_obj_delete(s_menu);
        s_menu = nullptr;
        s_card = nullptr;
    }
}

void show_busy(const char *text)
{
    lv_obj_clean(s_card);
    lv_obj_set_flex_align(s_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *spin = lv_spinner_create(s_card);
    lv_obj_set_size(spin, 96, 96);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, lv_color_hex(COLOR_CANCEL), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(COLOR_RESTART), LV_PART_INDICATOR);
    lv_obj_t *l = lv_label_create(s_card);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, phone_font_by_size(24), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);
}

void run_later(lv_timer_cb_t cb, uint32_t ms)
{
    lv_timer_t *t = lv_timer_create(cb, ms, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

void do_restart(lv_timer_t *)
{
    bsp_display_brightness_set(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void on_restart(lv_event_t *)
{
    ESP_LOGW(TAG, "Restart");
    show_busy(phone_tr("Đang khởi động lại…", "Restarting…"));
    run_later(do_restart, 600);
}

void on_cancel(lv_event_t *)
{
    close_menu();
}

void add_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_PCT(100), 64);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, phone_font_by_size(24), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(l);
}

void show_menu(phone_power_action_t action)
{
    if (s_menu) {
        return;
    }
    s_menu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(COLOR_DIM), 0);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_menu);
    s_card = card;
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    const char *title_text = action == PHONE_POWER_RESTART_LANG
                             ? phone_tr("Khởi động lại để áp dụng ngôn ngữ?", "Restart to apply language?")
                             : phone_tr("Khởi động lại?", "Restart?");
    lv_label_set_text(title, title_text);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_font(title, phone_font_by_size(28), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);

    add_button(card, phone_tr("Khởi động lại", "Restart"), COLOR_RESTART, on_restart);
    add_button(card, action == PHONE_POWER_RESTART_LANG ? phone_tr("Để sau", "Later") : phone_tr("Hủy", "Cancel"), COLOR_CANCEL, on_cancel);
}

} // namespace

extern "C" void phone_power_confirm(phone_power_action_t action)
{
    show_menu(action);
}
