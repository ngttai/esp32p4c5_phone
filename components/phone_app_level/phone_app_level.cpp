/*
 * Level app: a two-axis spirit level. The bubble floats toward the higher side of the board,
 * like the bubble of a real level, and turns green when the board lies flat.
 *
 * Only the accelerometer of the IMU is used. "Calibrate" stores the current reading as the
 * zero of both axes (NVS), to cancel the mounting offset of the sensor and of the case.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "LEVEL"
#include "esp_lib_utils.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"
#include "phone_app_level.hpp"

#define APP_NAME "Thước thủy"

extern "C" const lv_image_dsc_t phone_level_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_CARD = 0x1E252C;
constexpr uint32_t COLOR_LINE = 0x3A444E;
constexpr uint32_t COLOR_TEXT = 0xFFFFFF;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_BUBBLE = 0x3DDCFF;
constexpr uint32_t COLOR_LEVEL = 0x22C55E;
constexpr uint32_t COLOR_ERROR = 0xF87171;

constexpr int DIAL_D = 380;                 // outer ring diameter
constexpr int BUBBLE_D = 60;
constexpr int BUBBLE_MAX = DIAL_D / 2 - BUBBLE_D / 2 - 6;   // bubble center travel, px
constexpr float FULL_SCALE_DEG = 20.0f;     // tilt that puts the bubble on the ring
constexpr float LEVEL_TOL_DEG = 0.5f;       // both axes below this: level
constexpr float CAL_MAX_DEG = 10.0f;        // refuse to calibrate when clearly not lying flat
constexpr uint32_t PERIOD_MS = 40;
constexpr float SMOOTHING = 0.15f;          // low-pass of the accelerometer, per sample
constexpr float WAKE_DEG = 0.4f;            // tilt change that counts as "the user is using it"
constexpr float LSB_PER_G = 1000.0f / 0.061f;   // +-2 g range: 0.061 mg/LSB

/*
 * Sensor axes as seen on the screen (portrait, USB connector at the bottom).
 * An accelerometer at rest reads +1 g on the axis that points up, so the reading on an axis
 * grows when that side of the board is raised: the bubble moves the same way.
 */
constexpr int AXIS_RIGHT = 1;   // sensor Y points to the left of the screen
constexpr int SIGN_RIGHT = -1;
constexpr int AXIS_UP = 0;      // sensor X points to the bottom of the screen
constexpr int SIGN_UP = -1;

lv_obj_t *s_root;
lv_obj_t *s_bubble;
lv_obj_t *s_angle;
lv_obj_t *s_state;
lv_obj_t *s_axes;
bool s_imu_ok;
bool s_have_sample;
float s_right, s_up, s_z;       // filtered acceleration in g, screen axes, before calibration
float s_off_right, s_off_up;    // calibration zero, g
float s_wake_x, s_wake_y;       // tilt at the last activity trigger, degrees

float deg(float rad)
{
    return rad * 180.0f / static_cast<float>(M_PI);
}

void load_cal()
{
    s_off_right = phone_pref_get("lvl_ox", 0) / LSB_PER_G;
    s_off_up = phone_pref_get("lvl_oy", 0) / LSB_PER_G;
}

void save_cal(float right, float up)
{
    s_off_right = right;
    s_off_up = up;
    phone_pref_set("lvl_ox", static_cast<int>(lroundf(right * LSB_PER_G)));
    phone_pref_set("lvl_oy", static_cast<int>(lroundf(up * LSB_PER_G)));
}

void show_state(const char *text, uint32_t color)
{
    lv_label_set_text(s_state, text);
    lv_obj_set_style_text_color(s_state, lv_color_hex(color), 0);
}

void on_timer(lv_timer_t *)
{
    if (!s_root || !s_imu_ok) {
        return;
    }
    bsp_imu_raw_t raw;
    if (bsp_imu_read(&raw) != ESP_OK) {
        return;
    }
    const float right = SIGN_RIGHT * raw.accel[AXIS_RIGHT] / LSB_PER_G;
    const float up = SIGN_UP * raw.accel[AXIS_UP] / LSB_PER_G;
    const float z = raw.accel[2] / LSB_PER_G;
    if (!s_have_sample) {
        s_right = right;
        s_up = up;
        s_z = z;
        s_have_sample = true;
    } else {
        s_right += (right - s_right) * SMOOTHING;
        s_up += (up - s_up) * SMOOTHING;
        s_z += (z - s_z) * SMOOTHING;
    }

    const float gx = s_right - s_off_right;
    const float gy = s_up - s_off_up;
    const float tilt_x = deg(atan2f(gx, sqrtf(gy * gy + s_z * s_z)));   // right side up: positive
    const float tilt_y = deg(atan2f(gy, sqrtf(gx * gx + s_z * s_z)));   // top side up: positive
    const float tilt = deg(atan2f(sqrtf(gx * gx + gy * gy), fabsf(s_z)));

    /* The bubble goes to the higher side; screen y grows downward */
    float bx = tilt_x / FULL_SCALE_DEG * BUBBLE_MAX;
    float by = -tilt_y / FULL_SCALE_DEG * BUBBLE_MAX;
    const float r = sqrtf(bx * bx + by * by);
    if (r > BUBBLE_MAX) {
        bx *= BUBBLE_MAX / r;
        by *= BUBBLE_MAX / r;
    }
    lv_obj_align(s_bubble, LV_ALIGN_CENTER, static_cast<int32_t>(lroundf(bx)), static_cast<int32_t>(lroundf(by)));

    const bool level = fabsf(tilt_x) < LEVEL_TOL_DEG && fabsf(tilt_y) < LEVEL_TOL_DEG;
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(level ? COLOR_LEVEL : COLOR_BUBBLE), 0);
    const int t10 = static_cast<int>(lroundf(tilt * 10));
    const int x10 = static_cast<int>(lroundf(tilt_x * 10));
    const int y10 = static_cast<int>(lroundf(tilt_y * 10));
    lv_label_set_text_fmt(s_angle, "%d.%d°", t10 / 10, t10 % 10);
    lv_obj_set_style_text_color(s_angle, lv_color_hex(level ? COLOR_LEVEL : COLOR_TEXT), 0);
    show_state(level ? T("Cân bằng", "Level") : T("Chưa cân bằng", "Not level"), level ? COLOR_LEVEL : COLOR_SUB);
    lv_label_set_text_fmt(s_axes, "X  %c%d.%d°      Y  %c%d.%d°", x10 < 0 ? '-' : '+', abs(x10) / 10, abs(x10) % 10,
                          y10 < 0 ? '-' : '+', abs(y10) / 10, abs(y10) % 10);

    /* Moving the board is using the app: keep the screen on, unless it is locked */
    if (fabsf(tilt_x - s_wake_x) > WAKE_DEG || fabsf(tilt_y - s_wake_y) > WAKE_DEG) {
        s_wake_x = tilt_x;
        s_wake_y = tilt_y;
        if (!phone_lock_is_locked() && lv_obj_get_screen(s_root) == lv_screen_active()) {
            lv_display_trigger_activity(nullptr);
        }
    }
}

void on_calibrate(lv_event_t *)
{
    if (!s_imu_ok || !s_have_sample) {
        return;
    }
    const float tilt = deg(atan2f(sqrtf(s_right * s_right + s_up * s_up), fabsf(s_z)));
    if (tilt > CAL_MAX_DEG) {
        show_state(T("Hãy đặt máy nằm phẳng", "Lay the board flat first"), COLOR_ERROR);
        return;
    }
    save_cal(s_right, s_up);
}

void on_reset(lv_event_t *)
{
    save_cal(0, 0);
}

lv_obj_t *make_ring(lv_obj_t *parent, int d, int width, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_center(o);
    return o;
}

void make_line(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COLOR_LINE), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(o);
}

void make_button(lv_obj_t *parent, const char *text, uint32_t bg, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 56);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
}

void build_ui()
{
    s_have_sample = false;
    s_wake_x = s_wake_y = 0;

    lv_obj_t *root = lv_obj_create(lv_screen_active());
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(root, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(20), 0);
    lv_obj_set_style_pad_hor(root, 20, 0);
    lv_obj_set_style_pad_top(root, 16, 0);
    lv_obj_set_style_pad_bottom(root, 52, 0);     /* the buttons stay above the navigation bar */
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, T("Thước thủy", "Level"));
    lv_obj_set_style_text_font(title, phone_font_by_size(28), 0);

    lv_obj_t *dial = lv_obj_create(root);
    lv_obj_remove_style_all(dial);
    lv_obj_set_size(dial, DIAL_D, DIAL_D);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dial, lv_color_hex(COLOR_CARD), 0);
    lv_obj_remove_flag(dial, LV_OBJ_FLAG_SCROLLABLE);
    make_line(dial, DIAL_D, 2);
    make_line(dial, 2, DIAL_D);
    make_ring(dial, DIAL_D, 3, COLOR_LINE);
    make_ring(dial, DIAL_D / 2, 2, COLOR_LINE);
    make_ring(dial, BUBBLE_D + 12, 3, COLOR_LEVEL);     // target: the bubble fits inside when level

    s_bubble = lv_obj_create(dial);
    lv_obj_remove_style_all(s_bubble);
    lv_obj_set_size(s_bubble, BUBBLE_D, BUBBLE_D);
    lv_obj_set_style_radius(s_bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(COLOR_BUBBLE), 0);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_bubble);

    lv_obj_t *info = lv_obj_create(root);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(info, 4, 0);

    s_angle = lv_label_create(info);
    lv_obj_set_style_text_font(s_angle, &lv_font_montserrat_44, 0);
    lv_label_set_text(s_angle, "--");
    s_state = lv_label_create(info);
    lv_label_set_text(s_state, "");
    s_axes = lv_label_create(info);
    lv_obj_set_style_text_color(s_axes, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_axes, phone_font_by_size(18), 0);
    lv_label_set_text(s_axes, "");

    lv_obj_t *btns = lv_obj_create(root);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 14, 0);
    make_button(btns, T("Hiệu chỉnh", "Calibrate"), 0x2F80ED, on_calibrate);
    make_button(btns, T("Đặt lại", "Reset"), 0x2A333C, on_reset);

    if (!s_imu_ok) {
        show_state(T("Không thấy cảm biến IMU", "IMU not found"), COLOR_ERROR);
    }
}

} // namespace

namespace esp_brookesia::apps {

PhoneLevel *PhoneLevel::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneLevel();
    }
    return _instance;
}

PhoneLevel::PhoneLevel():
    App(phone_tr("Thước thủy", "Level"), &phone_level_icon, true, true, true)
{
}

PhoneLevel::~PhoneLevel()
{
}

bool PhoneLevel::run()
{
    s_imu_ok = bsp_imu_init() == ESP_OK;
    load_cal();
    build_ui();
    lv_timer_create(on_timer, PERIOD_MS, nullptr);
    return true;
}

bool PhoneLevel::back()
{
    return notifyCoreClosed();
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneLevel, APP_NAME, []()
{
    return std::shared_ptr<PhoneLevel>(PhoneLevel::requestInstance(), [](PhoneLevel * p) {});
})

} // namespace esp_brookesia::apps
