/*
 * SDK header for the ELF apps of the phone (API v3): short wrappers over phone_elf_api_t.
 *
 *   #include "phone_elf_sdk.h"
 *   int main(int argc, char *argv[])
 *   {
 *       if (phone_init(argc, argv) != 0) return -1;        // keeps the API table and the context of this app
 *       void *label = ui_label(ui_root());
 *       ui_text(label, "Hello");
 *       return 0;
 *   }
 *
 * The entry returns when the screen is built: everything after that happens in callbacks (buttons, timers, life cycle) on the
 * LVGL task, so keep them short. Details of every call: phone_elf_api.h.
 */
#pragma once
#include <stdio.h>
#include <string.h>
#include "phone_elf_api.h"

static const phone_elf_api_t *phone_api;
static phone_elf_ctx_t *phone_ctx;

/* any call of the API with the context filled in: PHONE(timer_delete, t) -> phone_api->timer_delete(phone_ctx, t) */
#define PHONE(fn, ...) (phone_api->fn(phone_ctx, ##__VA_ARGS__))

/* call first in main(): 0 when the firmware speaks the API version this SDK was written for */
static inline int phone_init(int argc, char *argv[])
{
    if (argc < 2) return -1;
    phone_api = (const phone_elf_api_t *)argv[0];
    phone_ctx = (phone_elf_ctx_t *)argv[1];
    return (phone_api && phone_ctx && phone_api->version == PHONE_ELF_API_VERSION) ? 0 : -1;
}

/* ---- core ---- */
static inline void app_log(const char *text) { PHONE(log, text); }
#define app_logf(...) do { char app_log_buf_[128]; snprintf(app_log_buf_, sizeof(app_log_buf_), __VA_ARGS__); app_log(app_log_buf_); } while (0)
static inline void app_close(void) { PHONE(close); }
static inline const char *app_tr(const char *vi, const char *en) { return PHONE(tr, vi, en); }
static inline uint32_t app_permissions(void) { return PHONE(permissions); }

/* ---- UI ---- */
static inline void *ui_root(void) { return PHONE(root); }
static inline void *ui_box(void *parent) { return PHONE(obj_create, parent); }
static inline void *ui_label(void *parent) { return PHONE(label_create, parent); }
static inline void ui_text(void *label, const char *text) { PHONE(label_set_text, label, text); }
static inline void *ui_slider(void *parent, int min, int max, int value)
{
    void *s = PHONE(slider_create, parent);
    PHONE(slider_set_range, s, min, max);
    PHONE(slider_set_value, s, value);
    return s;
}
static inline void *ui_switch(void *parent) { return PHONE(switch_create, parent); }
static inline void ui_size(void *obj, int w, int h) { PHONE(obj_set_size, obj, w, h); }
static inline void ui_pos(void *obj, int x, int y) { PHONE(obj_set_pos, obj, x, y); }
static inline void ui_align(void *obj, int align, int x, int y) { PHONE(obj_align, obj, align, x, y); }
static inline void ui_bg(void *obj, uint32_t rgb) { PHONE(obj_set_bg_color, obj, rgb); }
static inline void ui_color(void *obj, uint32_t rgb) { PHONE(obj_set_text_color, obj, rgb); }
static inline void ui_radius(void *obj, int r) { PHONE(obj_set_radius, obj, r); }
static inline void ui_hide(void *obj, bool hidden) { PHONE(obj_set_hidden, obj, hidden); }
static inline void ui_column(void *obj) { PHONE(obj_set_flex_column, obj); }
static inline void ui_row(void *obj) { PHONE(obj_set_flex_row, obj); }
static inline void ui_delete(void *obj) { PHONE(obj_delete, obj); }
static inline void ui_on(void *obj, int event, phone_elf_event_cb_t cb, void *user) { PHONE(obj_add_event_cb, obj, cb, event, user); }
static inline void *ui_event_target(void *event) { return phone_api->event_target(event); }
static inline void *ui_event_user(void *event) { return phone_api->event_user_data(event); }

/* a button with a label, clicked -> cb(event); returns the button */
static inline void *ui_button(void *parent, const char *text, phone_elf_event_cb_t cb, void *user)
{
    void *b = PHONE(button_create, parent);
    PHONE(obj_set_size, b, -45, 64);
    void *l = PHONE(label_create, b);
    PHONE(label_set_text, l, text);
    PHONE(obj_align, l, PHONE_ELF_ALIGN_CENTER, 0, 0);
    if (cb) PHONE(obj_add_event_cb, b, cb, PHONE_ELF_EV_CLICKED, user);
    return b;
}

/* ---- timers, time ---- */
static inline void *timer_start(uint32_t period_ms, phone_elf_timer_cb_t cb, void *user) { return PHONE(timer_create, period_ms, cb, user); }
static inline void timer_stop(void *timer) { PHONE(timer_delete, timer); }
static inline uint32_t app_millis(void) { return PHONE(millis); }
static inline int64_t app_epoch(void) { return PHONE(epoch); }

/* ---- settings and files ---- */
static inline int32_t pref_int(const char *key, int32_t def) { return PHONE(pref_get, key, def); }
static inline int pref_save(const char *key, int32_t value) { return PHONE(pref_set, key, value); }
static inline const char *app_data_dir(void) { return PHONE(app_dir); }
static inline const char *app_pkg_dir(void) { return PHONE(pkg_dir); }
/* a path inside the data folder of the app: path_in_data(buf, sizeof buf, "notes.txt") */
static inline void path_in_data(char *buf, size_t n, const char *name) { snprintf(buf, n, "%s/%s", PHONE(app_dir), name); }

/* ---- life cycle ---- */
static inline int app_lifecycle(void (*on_pause)(void *), void (*on_resume)(void *), void (*on_close)(void *), void *user, uint32_t flags)
{
    static phone_elf_lifecycle_t lc;
    lc.on_pause = on_pause; lc.on_resume = on_resume; lc.on_close = on_close; lc.user = user; lc.flags = flags;
    return PHONE(lifecycle_set, &lc);
}
static inline bool app_foreground(void) { return PHONE(is_foreground); }

/* ---- hardware (need the permission in manifest.json: they return PHONE_ELF_E_PERM without it) ---- */
static inline int hw_beep(void) { return PHONE(beep); }
static inline int hw_led(uint32_t rgb, int effect, int period_ms) { return PHONE(led_set, rgb, effect, period_ms); }
static inline int hw_led_off(void) { return PHONE(led_off); }
static inline int hw_imu(phone_elf_imu_t *out) { return PHONE(imu_read, out); }
static inline int hw_battery(phone_elf_battery_t *out) { return PHONE(battery_read, out); }
static inline int hw_online(void) { return PHONE(net_online); }
