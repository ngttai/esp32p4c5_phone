/*
 * LED RGB app: the RGB LED of the board as a small lamp. The effects are the ones of the LED service
 * (phone_led), the choices apply at once. The light follows the app: on while it runs (also from the home
 * screen and with the screen off), off once it is closed; nothing is stored. A request of higher priority
 * (the Recorder blinking red) shows over it and gives the LED back when it ends.
 */
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "esp_timer.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "LED_RGB"
#include "esp_lib_utils.h"
#include "phone_fonts.h"
#include "phone_led.h"
#include "phone_locale.h"
#include "phone_app_led_rgb.hpp"

#define APP_NAME "Đèn RGB"

extern "C" const lv_image_dsc_t phone_led_rgb_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

constexpr const char *OWNER = "LED RGB";

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_CARD = 0x1E252C;
constexpr uint32_t COLOR_TEXT = 0xFFFFFF;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0x2F80ED;
constexpr uint32_t COLOR_WARN = 0xF87171;
constexpr uint32_t COLOR_OFF_TEXT = 0x5B6570;   // labels of a control that does not apply
constexpr uint32_t COLOR_OFF_KNOB = 0x6B7785;
constexpr lv_style_selector_t DISABLED = LV_STATE_DISABLED;

constexpr int PREVIEW_D = 112;
constexpr int SWATCH_D = 44;
constexpr int HUE_H = 28;
constexpr int HUE_STEPS = 48;
constexpr uint32_t SWATCHES[] = {0xFFFFFF, 0xEF4444, 0xF97316, 0xEAB308, 0x22C55E, 0x06B6D4, 0x3B82F6, 0x7C3AED};
constexpr int PERIOD_SLOW = 4000;   // ms at speed 0
constexpr int PERIOD_FAST = 300;    // ms at speed 100

/* the choices, kept in RAM while the phone runs (not stored) */
bool s_on;                      // off each time the app opens
phone_led_effect_t s_effect = PHONE_LED_SOLID;
uint32_t s_color = 0xFFFFFF;
int s_speed = 50;
int64_t s_started_us;           // when the request began: the preview follows the same phase

lv_obj_t *s_root;
lv_obj_t *s_preview;
lv_obj_t *s_title;
lv_obj_t *s_sub;
lv_obj_t *s_switch;
lv_obj_t *s_effect_btns[PHONE_LED_EFFECT_COUNT];
lv_obj_t *s_color_box;          // swatches + hue bar
lv_obj_t *s_swatches[sizeof(SWATCHES) / sizeof(SWATCHES[0])];
lv_obj_t *s_hue;
lv_obj_t *s_hue_knob;
lv_obj_t *s_speed_box;
lv_obj_t *s_speed_val;
lv_obj_t *s_bri_val;

int period_ms()
{
    return PERIOD_SLOW - (PERIOD_SLOW - PERIOD_FAST) * s_speed / 100;
}

phone_led_pattern_t pattern()
{
    const int period = period_ms();
    return {s_effect, s_color, static_cast<uint16_t>(period), static_cast<uint16_t>(period / 3)};
}

void apply()
{
    if (s_on) {
        const phone_led_pattern_t p = pattern();
        phone_led_request(OWNER, PHONE_LED_AMBIENT, &p);
        s_started_us = esp_timer_get_time();
    } else {
        phone_led_release(OWNER);
    }
}

uint32_t hue_rgb(float h)
{
    const float x = 1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h / 60.0f) % 6) {
    case 0: r = 1; g = x; break;
    case 1: r = x; g = 1; break;
    case 2: g = 1; b = x; break;
    case 3: g = x; b = 1; break;
    case 4: r = x; b = 1; break;
    default: r = 1; b = x; break;
    }
    return (static_cast<uint32_t>(r * 255) << 16) | (static_cast<uint32_t>(g * 255) << 8) | static_cast<uint32_t>(b * 255);
}

const char *speed_name()
{
    return s_speed < 34 ? T("Chậm", "Slow") : s_speed < 67 ? T("Vừa", "Medium") : T("Nhanh", "Fast");
}

/* What the LED shows, roughly: the preview runs the same effect at the same phase */
void update_preview()
{
    uint32_t c = s_color;
    float level = 1.0f;
    const int64_t t = (esp_timer_get_time() - s_started_us) / 1000;
    const int period = period_ms();
    switch (s_effect) {
    case PHONE_LED_BLINK:
        level = (t % period) < period / 3 ? 1.0f : 0.15f;
        break;
    case PHONE_LED_BREATHE:
        level = 0.15f + 0.85f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * (t % period) / period)) / 2.0f;
        break;
    case PHONE_LED_RAINBOW:
        c = hue_rgb(360.0f * (t % period) / period);
        break;
    default:
        break;
    }
    if (!s_on) {
        c = 0x3A444E;
        level = 1.0f;
    }
    /* redrawn only when it changes: a solid light costs nothing; no shadow (blurred in software every frame) */
    static uint32_t shown_c = 0xFFFFFFFF;
    static lv_opa_t shown_opa;
    const lv_opa_t opa = static_cast<lv_opa_t>(LV_OPA_COVER * level);
    if (c == shown_c && opa == shown_opa) {
        return;
    }
    shown_c = c;
    shown_opa = opa;
    lv_obj_set_style_bg_color(s_preview, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(s_preview, opa, 0);
    lv_obj_set_style_outline_color(s_preview, lv_color_hex(c), 0);
    lv_obj_set_style_outline_opa(s_preview, s_on ? static_cast<lv_opa_t>(LV_OPA_30 * level) : LV_OPA_TRANSP, 0);
}

void update_texts()
{
    char shown[24];
    phone_led_shown_owner(shown, sizeof(shown));
    const bool other = s_on && shown[0] && strcmp(shown, OWNER) != 0;
    lv_label_set_text(s_title, !s_on ? T("Đang tắt", "Off") : T("Đang sáng", "On"));
    if (other) {
        lv_label_set_text_fmt(s_sub, T("%s đang dùng đèn", "%s is using the LED"),
                              strcmp(shown, "Recorder") == 0 ? T("Ghi âm", "Recorder") : shown);
        lv_obj_set_style_text_color(s_sub, lv_color_hex(COLOR_WARN), 0);
    } else {
        const phone_led_effect_info_t *info = phone_led_effect_info(s_effect);
        lv_label_set_text_fmt(s_sub, "%s · %d%%", phone_tr(info->name_vi, info->name_en), phone_led_brightness());
        lv_obj_set_style_text_color(s_sub, lv_color_hex(COLOR_SUB), 0);
    }
}

/* Dimmed and not touchable: the disabled state goes on every control inside (a parent's does not stop them) */
void set_disabled_tree(lv_obj_t *obj, bool disabled)
{
    if (disabled) {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        set_disabled_tree(lv_obj_get_child(obj, i), disabled);
    }
}

/* Not applying to the effect: greyed by the disabled styles of each control (an opacity on the box would show
 * the track through the knob) and not touchable */
void set_enabled(lv_obj_t *box, bool enabled)
{
    set_disabled_tree(box, !enabled);
}

void update_controls()
{
    for (int i = 0; i < PHONE_LED_EFFECT_COUNT; i++) {
        lv_obj_set_style_bg_color(s_effect_btns[i], lv_color_hex(i == s_effect ? COLOR_ACCENT : COLOR_CARD), 0);
    }
    for (size_t i = 0; i < sizeof(SWATCHES) / sizeof(SWATCHES[0]); i++) {
        lv_obj_set_style_outline_width(s_swatches[i], SWATCHES[i] == s_color ? 3 : 0, 0);
    }
    const phone_led_effect_info_t *info = phone_led_effect_info(s_effect);
    set_enabled(s_color_box, info->uses_color);
    lv_obj_invalidate(s_hue);       /* drawn by us: the disabled state changes no style, so no redraw by itself */
    set_enabled(s_speed_box, info->uses_period);
    lv_label_set_text(s_speed_val, speed_name());
    lv_label_set_text_fmt(s_bri_val, "%d%%", phone_led_brightness());
    if (s_on) {
        lv_obj_add_state(s_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_switch, LV_STATE_CHECKED);
    }
    update_texts();
}

void on_timer(lv_timer_t *)
{
    if (!s_root) {
        return;
    }
    update_preview();
    update_texts();
}

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_OFF_TEXT), DISABLED);
    lv_obj_set_style_text_font(l, phone_font_by_size(18), 0);
    return l;
}

/* A row "caption ... value" above a slider */
lv_obj_t *slider_block(lv_obj_t *parent, const char *text, lv_obj_t **value, int initial, lv_event_cb_t cb)
{
    lv_obj_t *box = plain(parent);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 14, 0);
    lv_obj_set_style_pad_bottom(box, 10, 0);       /* the knob is taller than the track: not cut */
    lv_obj_t *row = plain(box);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    caption(row, text);
    *value = lv_label_create(row);
    lv_obj_set_style_text_font(*value, phone_font_by_size(18), 0);
    lv_obj_set_style_text_color(*value, lv_color_hex(COLOR_OFF_TEXT), DISABLED);
    lv_obj_align(*value, LV_ALIGN_RIGHT_MID, 0, 0);
    /* the knob reaches past both ends of the track: the track sits inset in a holder */
    lv_obj_t *holder = plain(box);
    lv_obj_set_size(holder, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(holder, 32, 0);
    lv_obj_set_style_pad_ver(holder, 10, 0);
    lv_obj_t *slider = lv_slider_create(holder);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3A444E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A333C), LV_PART_MAIN | DISABLED);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x4A5560), LV_PART_INDICATOR | DISABLED);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_OFF_KNOB), LV_PART_KNOB | DISABLED);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB | DISABLED);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return box;
}

void on_switch(lv_event_t *e)
{
    s_on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    apply();
    update_controls();
}

void on_effect(lv_event_t *e)
{
    s_effect = static_cast<phone_led_effect_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    apply();
    update_controls();
}

void on_swatch(lv_event_t *e)
{
    s_color = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    apply();
    update_controls();
}

/* The bar is drawn HUE_H / 2 in from both ends of s_hue, so the knob always stays inside it (nothing to clip);
 * x counts from the start of the bar */
constexpr int HUE_INSET = HUE_H / 2;

void place_hue_knob(int x)
{
    lv_obj_set_x(s_hue_knob, x);
}

void on_hue(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(s_hue, &a);
    const int w = lv_area_get_width(&a) - 2 * HUE_INSET;
    int x = p.x - a.x1 - HUE_INSET;
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    place_hue_knob(x);
    s_color = hue_rgb(360.0f * x / w);
    apply();
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        update_controls();
    }
}

void on_hue_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(s_hue, &a);
    const int w = lv_area_get_width(&a) - 2 * HUE_INSET;
    const int x0 = a.x1 + HUE_INSET;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = lv_obj_has_state(s_hue, LV_STATE_DISABLED) ? LV_OPA_30 : LV_OPA_COVER;
    for (int i = 0; i < HUE_STEPS; i++) {
        lv_area_t r = {x0 + w * i / HUE_STEPS, a.y1 + 6, x0 + w * (i + 1) / HUE_STEPS - 1, a.y2 - 6};
        dsc.bg_color = lv_color_hex(hue_rgb(360.0f * (i + 0.5f) / HUE_STEPS));
        dsc.radius = (i == 0 || i == HUE_STEPS - 1) ? 6 : 0;
        lv_draw_rect(layer, &dsc, &r);
    }
}

void on_speed(lv_event_t *e)
{
    s_speed = lv_slider_get_value(lv_event_get_target_obj(e));
    lv_label_set_text(s_speed_val, speed_name());
    apply();
}

void on_brightness(lv_event_t *e)
{
    phone_led_set_brightness(std::max(1, static_cast<int>(lv_slider_get_value(lv_event_get_target_obj(e)))));
    lv_label_set_text_fmt(s_bri_val, "%d%%", phone_led_brightness());
}

void build_ui()
{
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(root, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(20), 0);
    lv_obj_set_style_pad_hor(root, 18, 0);
    lv_obj_set_style_pad_top(root, 16, 0);
    lv_obj_set_style_pad_bottom(root, 40, 0);
    lv_obj_set_style_pad_row(root, 14, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    /* preview card: the light, what it does, the switch */
    lv_obj_t *card = plain(root);
    lv_obj_set_size(card, LV_PCT(100), PREVIEW_D + 40);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(card, 18, 0);
    s_preview = plain(card);
    lv_obj_set_size(s_preview, PREVIEW_D, PREVIEW_D);
    lv_obj_set_style_radius(s_preview, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_outline_width(s_preview, 8, 0);
    lv_obj_set_style_outline_pad(s_preview, 2, 0);
    lv_obj_align(s_preview, LV_ALIGN_LEFT_MID, 20, 0);
    s_title = lv_label_create(card);
    lv_obj_set_style_text_font(s_title, phone_font_by_size(24), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, PREVIEW_D + 40, 40);
    s_sub = lv_label_create(card);
    lv_obj_set_style_text_font(s_sub, phone_font_by_size(18), 0);
    lv_obj_set_width(s_sub, 170);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_sub, LV_ALIGN_TOP_LEFT, PREVIEW_D + 40, 76);
    s_switch = lv_switch_create(card);
    lv_obj_set_size(s_switch, 64, 36);
    lv_obj_align(s_switch, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(s_switch, lv_color_hex(COLOR_ACCENT),
                              static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    lv_obj_add_event_cb(s_switch, on_switch, LV_EVENT_VALUE_CHANGED, nullptr);

    /* effects, as the service lists them */
    caption(root, T("Hiệu ứng", "Effect"));
    lv_obj_t *grid = plain(root);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    for (int i = 0; i < PHONE_LED_EFFECT_COUNT; i++) {
        const phone_led_effect_info_t *info = phone_led_effect_info(static_cast<phone_led_effect_t>(i));
        lv_obj_t *b = lv_button_create(grid);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 212, 56);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(b, on_effect, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, phone_tr(info->name_vi, info->name_en));
        lv_obj_center(l);
        s_effect_btns[i] = b;
    }

    /* color: quick swatches and a hue bar */
    s_color_box = plain(root);
    lv_obj_set_size(s_color_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_color_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_color_box, 12, 0);
    caption(s_color_box, T("Màu", "Color"));
    lv_obj_t *sw_row = plain(s_color_box);
    lv_obj_set_size(sw_row, LV_PCT(100), SWATCH_D + 16);     /* room for the ring of the chosen color (outline pad 3 + width 3) */
    lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(sw_row, 8, 0);
    for (size_t i = 0; i < sizeof(SWATCHES) / sizeof(SWATCHES[0]); i++) {
        lv_obj_t *s = lv_button_create(sw_row);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, SWATCH_D, SWATCH_D);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(SWATCHES[i]), 0);
        lv_obj_set_style_outline_color(s, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_outline_pad(s, 3, 0);
        lv_obj_set_style_opa(s, LV_OPA_30, DISABLED);
        lv_obj_add_event_cb(s, on_swatch, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(SWATCHES[i])));
        s_swatches[i] = s;
    }
    /* inset like the sliders below: the knob has room at both ends */
    lv_obj_t *hue_holder = plain(s_color_box);
    lv_obj_set_size(hue_holder, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(hue_holder, 18, 0);
    s_hue = plain(hue_holder);
    lv_obj_set_size(s_hue, LV_PCT(100), HUE_H);
    lv_obj_add_flag(s_hue, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hue, on_hue_draw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(s_hue, on_hue, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_hue, on_hue, LV_EVENT_RELEASED, nullptr);
    s_hue_knob = plain(s_hue);
    lv_obj_set_size(s_hue_knob, HUE_H, HUE_H);
    lv_obj_set_style_radius(s_hue_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_hue_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_hue_knob, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_hue_knob, lv_color_hex(COLOR_OFF_KNOB), DISABLED);
    lv_obj_remove_flag(s_hue_knob, LV_OBJ_FLAG_CLICKABLE);
    place_hue_knob(0);

    s_speed_box = slider_block(root, T("Tốc độ", "Speed"), &s_speed_val, s_speed, on_speed);
    slider_block(root, T("Độ sáng", "Brightness"), &s_bri_val, phone_led_brightness(), on_brightness);

    lv_obj_t *note = lv_label_create(root);
    lv_label_set_text(note, T("Đóng app thì đèn tắt", "Closing the app turns the light off"));
    lv_obj_set_style_text_color(note, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(note, phone_font_by_size(16), 0);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);

    update_controls();
    update_preview();
}

} // namespace

namespace esp_brookesia::apps {

PhoneLedRgb *PhoneLedRgb::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneLedRgb();
    }
    return _instance;
}

PhoneLedRgb::PhoneLedRgb():
    App(phone_tr("Đèn RGB", "LED RGB"), &phone_led_rgb_icon, true, true, phone_nav_bar())
{
}

PhoneLedRgb::~PhoneLedRgb()
{
}

bool PhoneLedRgb::run()
{
    s_on = false;       /* the light starts off: the switch turns it on */
    apply();
    build_ui();
    lv_timer_create(on_timer, 40, nullptr);
    return true;
}

bool PhoneLedRgb::back()
{
    return notifyCoreClosed();
}

bool PhoneLedRgb::keepRunning() const
{
    return s_on;
}

/* The light follows the app */
bool PhoneLedRgb::close()
{
    phone_led_release(OWNER);
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneLedRgb, APP_NAME, []()
{
    return std::shared_ptr<PhoneLedRgb>(PhoneLedRgb::requestInstance(), [](PhoneLedRgb * p) {});
})

} // namespace esp_brookesia::apps
