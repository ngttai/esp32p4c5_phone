#include "phone_power.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"

#define TAG "PWR"

namespace {

constexpr int BATTERY_POLL_MS = 5000;

constexpr uint32_t COLOR_DIM = 0x000000;
constexpr uint32_t COLOR_CARD = 0x1C252E;
constexpr uint32_t COLOR_TEXT = 0xFFFFFF;
constexpr uint32_t COLOR_OFF = 0xE5484D;
constexpr uint32_t COLOR_RESTART = 0x2F80ED;
constexpr uint32_t COLOR_CANCEL = 0x3A4652;

/* Low battery: warnings at 15 % and 5 %, power off below 2 % (or 3.3 V) when no USB supply keeps the board up.
 * Without a battery the board runs from USB and none of this applies. */
constexpr int LOW_WARN_PCT[] = {15, 5};
constexpr int LOW_REARM_PCT = 3;             // a warning comes back after rising this much above its level
constexpr int CRITICAL_PCT = 2;
constexpr int CRITICAL_MV = 3300;
constexpr int CRITICAL_READS = 3;            // consecutive readings (15 s), so a load dip does not power off
constexpr int CRITICAL_COUNTDOWN_S = 10;

lv_obj_t *s_menu;
lv_obj_t *s_card;
lv_timer_t *s_countdown;           // LVGL context
lv_obj_t *s_countdown_label;
int s_countdown_left;

/* poll task only */
bool s_warned[sizeof(LOW_WARN_PCT) / sizeof(LOW_WARN_PCT[0])];
int s_critical_reads;
bool s_critical_shown;

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
bsp_battery_info_t s_battery;      // guarded by s_lock
bool s_battery_valid;              // guarded by s_lock
TaskHandle_t s_poll_task;

void close_menu()
{
    if (s_countdown) {
        lv_timer_delete(s_countdown);
        s_countdown = nullptr;
        s_countdown_label = nullptr;
    }
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

void do_off(lv_timer_t *)
{
    bsp_power_off();
    ESP_LOGE(TAG, "Power off failed");
    close_menu();
}

void on_off(lv_event_t *)
{
    ESP_LOGW(TAG, "Power off");
    if (s_countdown) {
        lv_timer_delete(s_countdown);
        s_countdown = nullptr;
        s_countdown_label = nullptr;
    }
    show_busy(phone_tr("Đang tắt nguồn…", "Powering off…"));
    run_later(do_off, 400);
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

/* Dimmed full-screen layer with a card; on the system layer, so also above the lock screen */
lv_obj_t *open_card(const char *title_text)
{
    s_menu = lv_obj_create(lv_layer_sys());
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
    lv_label_set_text(title, title_text);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_font(title, phone_font_by_size(28), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    return card;
}

void show_menu(phone_power_action_t action)
{
    if (s_menu) {
        return;
    }
    const char *title_text = phone_tr("Nguồn", "Power");
    if (action == PHONE_POWER_OFF) {
        title_text = phone_tr("Tắt nguồn?", "Power off?");
    } else if (action == PHONE_POWER_RESTART) {
        title_text = phone_tr("Khởi động lại?", "Restart?");
    } else if (action == PHONE_POWER_RESTART_LANG) {
        title_text = phone_tr("Khởi động lại để áp dụng ngôn ngữ?", "Restart to apply language?");
    } else if (action == PHONE_POWER_RESTART_NAV) {
        title_text = phone_tr("Khởi động lại để áp dụng cách điều hướng?", "Restart to apply the navigation?");
    }
    lv_obj_t *card = open_card(title_text);

    if (action == PHONE_POWER_MENU || action == PHONE_POWER_OFF) {
        add_button(card, phone_tr("Tắt nguồn", "Power off"), COLOR_OFF, on_off);
    }
    if (action != PHONE_POWER_OFF) {
        add_button(card, phone_tr("Khởi động lại", "Restart"), COLOR_RESTART, on_restart);
    }
    const bool later = action == PHONE_POWER_RESTART_LANG || action == PHONE_POWER_RESTART_NAV;
    add_button(card, later ? phone_tr("Để sau", "Later") : phone_tr("Hủy", "Cancel"), COLOR_CANCEL, on_cancel);
}

/* ---------------- low battery (LVGL context) ---------------- */

lv_obj_t *add_text(lv_obj_t *card, const char *text)
{
    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, phone_font_by_size(22), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xB8C2CC), 0);
    return l;
}

void show_low_warning(int percent, bool usb)
{
    if (s_menu) {
        return;     // the power menu or another dialog is up: skip this warning
    }
    char body[160];
    if (usb) {
        snprintf(body, sizeof(body), phone_tr("Còn %d%%. Nguồn USB không đủ, pin vẫn đang giảm.",
                                              "%d%% left. The USB supply is too weak, the battery is still draining."),
                 percent);
    } else {
        snprintf(body, sizeof(body), phone_tr("Còn %d%%. Hãy cắm sạc.", "%d%% left. Plug in the charger."), percent);
    }
    lv_obj_t *card = open_card(phone_tr("Pin yếu", "Low battery"));
    add_text(card, body);
    add_button(card, "OK", COLOR_CANCEL, on_cancel);
}

void countdown_text()
{
    char buf[160];
    snprintf(buf, sizeof(buf), phone_tr("Máy sẽ tắt sau %d giây. Cắm sạc để tiếp tục dùng.",
                                        "Powering off in %d s. Plug in the charger to keep using it."),
             s_countdown_left);
    lv_label_set_text(s_countdown_label, buf);
}

void countdown_tick(lv_timer_t *)
{
    /* keep the screen on during the countdown */
    lv_display_trigger_activity(nullptr);
    if (--s_countdown_left > 0) {
        countdown_text();
        return;
    }
    lv_timer_delete(s_countdown);
    s_countdown = nullptr;
    ESP_LOGW(TAG, "Battery empty, power off");
    show_busy(phone_tr("Đang tắt nguồn…", "Powering off…"));
    run_later(do_off, 400);
}

void show_critical()
{
    close_menu();
    lv_obj_t *card = open_card(phone_tr("Pin sắp cạn", "Battery empty"));
    s_countdown_left = CRITICAL_COUNTDOWN_S;
    s_countdown_label = add_text(card, "");
    countdown_text();
    add_button(card, phone_tr("Tắt ngay", "Power off now"), COLOR_OFF, on_off);
    s_countdown = lv_timer_create(countdown_tick, 1000, nullptr);
}

/* ---------------- low battery (poll task) ---------------- */

void check_low_battery(const bsp_battery_info_t &b)
{
    /* No battery (runs from USB) or charging: nothing to warn about; a countdown on screen is cancelled */
    const bool draining = b.state == BSP_BATTERY_DISCHARGING && b.percent >= 0;
    if (!draining) {
        s_critical_reads = 0;
        for (bool &w : s_warned) {
            w = false;
        }
        if (s_critical_shown) {
            s_critical_shown = false;
            ESP_LOGI(TAG, "Power off cancelled (%s)", b.state == BSP_BATTERY_ABSENT ? "no battery" : "charging");
            if (bsp_display_lock(1000)) {
                close_menu();
                bsp_display_unlock();
            }
        }
        return;
    }

    /* Power off only without USB: with a weak USB supply the PMIC would start again at once */
    const bool critical = b.percent <= CRITICAL_PCT || (b.voltage_mv > 0 && b.voltage_mv < CRITICAL_MV);
    s_critical_reads = critical && !b.vbus_present ? s_critical_reads + 1 : 0;
    if (s_critical_reads >= CRITICAL_READS) {
        if (!s_critical_shown) {
            s_critical_shown = true;
            ESP_LOGW(TAG, "Battery critical: %d%% %u mV", b.percent, b.voltage_mv);
            phone_lock_wake();
            if (bsp_display_lock(1000)) {
                show_critical();
                bsp_display_unlock();
            }
        }
        return;
    }
    if (s_critical_shown && s_critical_reads == 0) {
        /* USB came back (weak) or the reading recovered */
        s_critical_shown = false;
        if (bsp_display_lock(1000)) {
            close_menu();
            bsp_display_unlock();
        }
    }

    /* Warnings: the lowest level reached, once until the battery rises above it again */
    int level = -1;
    for (size_t i = 0; i < sizeof(LOW_WARN_PCT) / sizeof(LOW_WARN_PCT[0]); i++) {
        if (b.percent > LOW_WARN_PCT[i] + LOW_REARM_PCT) {
            s_warned[i] = false;
        }
        if (b.percent <= LOW_WARN_PCT[i]) {
            level = static_cast<int>(i);
        }
    }
    if (level >= 0 && !s_warned[level]) {
        for (int i = 0; i <= level; i++) {
            s_warned[i] = true;     // reaching 5 % also covers the 15 % warning
        }
        ESP_LOGW(TAG, "Battery low: %d%% %u mV%s", b.percent, b.voltage_mv, b.vbus_present ? " (USB)" : "");
        phone_lock_wake();
        if (bsp_display_lock(1000)) {
            show_low_warning(b.percent, b.vbus_present);
            bsp_display_unlock();
        }
    }
}

void poll_task(void *)
{
    for (;;) {
        bsp_battery_info_t info;
        if (bsp_battery_get_info(&info) == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_battery = info;
            s_battery_valid = true;
            portEXIT_CRITICAL(&s_lock);
            check_low_battery(info);
        }
        /* Woken early by USB and charger events */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BATTERY_POLL_MS));
    }
}

/* Called from the BSP PMIC task */
void on_pmic_event(bsp_pmic_event_t event, void *)
{
    switch (event) {
    case BSP_PMIC_EVENT_KEY_SHORT_PRESS:
        /* lock screen, then screen off */
        phone_lock_on_power_key();
        break;
    case BSP_PMIC_EVENT_KEY_LONG_PRESS:
        phone_lock_wake();
        if (bsp_display_lock(1000)) {
            show_menu(PHONE_POWER_MENU);
            bsp_display_unlock();
        }
        break;
    case BSP_PMIC_EVENT_VBUS_INSERTED:
        xTaskNotifyGive(s_poll_task);
        phone_lock_on_charger();
        break;
    default:
        xTaskNotifyGive(s_poll_task);
        break;
    }
}

} // namespace

extern "C" void phone_power_start(void)
{
    if (s_poll_task) {
        return;
    }
    if (xTaskCreate(poll_task, "battery", 3072, nullptr, 3, &s_poll_task) != pdPASS) {
        ESP_LOGE(TAG, "Battery task create failed");
        return;
    }
    if (bsp_pmic_event_register(on_pmic_event, nullptr) != ESP_OK) {
        ESP_LOGW(TAG, "PMIC events unavailable (power key, USB)");
    }
}

extern "C" bool phone_power_get_battery(bsp_battery_info_t *out)
{
    portENTER_CRITICAL(&s_lock);
    bool valid = s_battery_valid;
    if (valid) {
        *out = s_battery;
    }
    portEXIT_CRITICAL(&s_lock);
    return valid;
}

extern "C" void phone_power_confirm(phone_power_action_t action)
{
    show_menu(action);
}
