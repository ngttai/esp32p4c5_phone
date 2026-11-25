#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_elf.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "phone_elf_api.h"
#include "phone_elf_host.h"
#include "phone_paths.h"
#include "phone_locale.h"
#include "phone_audio.h"
#include "phone_led.h"
#include "phone_power.h"
#include "phone_net.h"

static const char *TAG = "ELFHOST";

static bool s_active;
static esp_elf_t s_elf;
static lv_obj_t *s_root;
static char s_id[40];
static char s_data_dir[96];
static char s_pref_prefix[8];       /* "e" + 3 hex digits of the app id + "_" */
static phone_elf_host_close_cb_t s_close_cb;
static void *s_close_user;
static bool s_registered;

/* ---- API implementation ---- */
static void api_log(const char *text) { ESP_LOGI(TAG, "[%s] %s", s_id, text); }
static void close_async(void *arg) { (void)arg; if (s_active && s_close_cb) s_close_cb(s_close_user); }
static void api_close(void) { lv_async_call(close_async, NULL); }   /* after the callback of the app has returned */
static const char *api_tr(const char *vi, const char *en) { return phone_tr(vi, en); }

static void *api_root(void) { return s_root; }
static void *api_obj_create(void *p) { lv_obj_t *o = lv_obj_create(p); lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(o, 0, 0); lv_obj_set_style_pad_all(o, 0, 0); lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF); return o; }
static void *api_label_create(void *p) { lv_obj_t *l = lv_label_create(p); lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP); return l; }
static void *api_button_create(void *p) { return lv_button_create(p); }
static void *api_slider_create(void *p) { return lv_slider_create(p); }
static void api_label_set_text(void *l, const char *t) { lv_label_set_text(l, t); }
static int32_t dim(int v) { return v == -1 ? LV_SIZE_CONTENT : v < 0 ? lv_pct(-v) : v; }
static void api_obj_set_size(void *o, int w, int h) { lv_obj_set_size(o, dim(w), dim(h)); }
static void api_obj_align(void *o, int a, int x, int y)
{
    lv_align_t al = a == PHONE_ELF_ALIGN_TOP_MID ? LV_ALIGN_TOP_MID : a == PHONE_ELF_ALIGN_BOTTOM_MID ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_CENTER;
    lv_obj_align(o, al, x, y);
}
static void api_obj_set_bg_color(void *o, uint32_t rgb) { lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0); }
static void api_obj_set_text_color(void *o, uint32_t rgb) { lv_obj_set_style_text_color(o, lv_color_hex(rgb), 0); }
static void api_obj_set_flex_column(void *o) { lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN); }
static void api_obj_set_flex_row(void *o) { lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER); }
static void api_obj_add_event_cb(void *o, phone_elf_event_cb_t cb, int ev, void *ud)
{
    lv_obj_add_event_cb(o, (lv_event_cb_t)cb, ev == PHONE_ELF_EV_VALUE_CHANGED ? LV_EVENT_VALUE_CHANGED : LV_EVENT_CLICKED, ud);
}
static void *api_event_target(void *e) { return lv_event_get_target(e); }
static void *api_event_user_data(void *e) { return lv_event_get_user_data(e); }
static void api_slider_set_range(void *s, int a, int b) { lv_slider_set_range(s, a, b); }
static void api_slider_set_value(void *s, int v) { lv_slider_set_value(s, v, LV_ANIM_OFF); }
static int api_slider_get_value(void *s) { return lv_slider_get_value(s); }

static uint32_t api_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static int64_t api_epoch(void) { time_t t = time(NULL); return t > 1700000000 ? (int64_t)t : 0; }
static void api_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static const char *api_app_dir(void) { return s_data_dir; }
static int api_make_dirs(const char *path)
{
    /* only below the data folder of the app */
    size_t n = strlen(s_data_dir);
    if (strncmp(path, s_data_dir, n) != 0 || path[n] != '/' || strstr(path, "..")) {
        return -1;
    }
    return phone_sd_make_dirs(path);
}

static int api_pref_get(const char *key, int def)
{
    char k[32];
    snprintf(k, sizeof(k), "%s%.9s", s_pref_prefix, key);
    return phone_pref_get(k, def);
}
static void api_pref_set(const char *key, int v)
{
    char k[32];
    snprintf(k, sizeof(k), "%s%.9s", s_pref_prefix, key);
    phone_pref_set(k, v);
}

static void api_beep(void) { phone_audio_beep(); }
static int api_volume_get(void) { return phone_audio_get_volume(); }
static void api_led_set(uint32_t rgb, int effect, int period_ms)
{
    phone_led_pattern_t p = {
        .effect = effect == PHONE_ELF_LED_BLINK ? PHONE_LED_BLINK : effect == PHONE_ELF_LED_BREATHE ? PHONE_LED_BREATHE :
                  effect == PHONE_ELF_LED_RAINBOW ? PHONE_LED_RAINBOW : PHONE_LED_SOLID,
        .color = rgb, .period_ms = period_ms > 0 ? period_ms : 1000, .on_ms = 300,
    };
    phone_led_request(s_id, PHONE_LED_AMBIENT, &p);
}
static void api_led_off(void) { phone_led_release(s_id); }
static int api_imu_read(phone_elf_imu_t *out)
{
    static bool inited;
    if (!inited) { bsp_imu_init(); inited = true; vTaskDelay(pdMS_TO_TICKS(50)); }   /* the first sample comes after the first period */
    bsp_imu_raw_t raw = {0};
    for (int i = 0; i < 5; i++) {
        if (bsp_imu_read(&raw) != ESP_OK) return -1;
        if (raw.accel[0] || raw.accel[1] || raw.accel[2]) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    memcpy(out->gyro, raw.gyro, sizeof(out->gyro));
    memcpy(out->accel, raw.accel, sizeof(out->accel));
    return 0;
}
static int api_battery_read(phone_elf_battery_t *out)
{
    bsp_battery_info_t b;
    if (!phone_power_get_battery(&b)) return -1;
    out->percent = b.percent; out->voltage_mv = b.voltage_mv; out->charging_usb = b.vbus_present;
    return 0;
}
static bool api_net_online(void) { return phone_net_online(); }
static uint32_t api_free_psram(void) { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
static uint32_t api_free_sram(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

static const phone_elf_api_t s_api = {
    .version = PHONE_ELF_API_VERSION,
    .log = api_log, .close = api_close, .tr = api_tr,
    .root = api_root, .obj_create = api_obj_create, .label_create = api_label_create, .button_create = api_button_create,
    .slider_create = api_slider_create, .label_set_text = api_label_set_text, .obj_set_size = api_obj_set_size,
    .obj_align = api_obj_align, .obj_set_bg_color = api_obj_set_bg_color, .obj_set_text_color = api_obj_set_text_color,
    .obj_set_flex_column = api_obj_set_flex_column, .obj_set_flex_row = api_obj_set_flex_row,
    .obj_add_event_cb = api_obj_add_event_cb, .event_target = api_event_target, .event_user_data = api_event_user_data,
    .slider_set_range = api_slider_set_range, .slider_set_value = api_slider_set_value, .slider_get_value = api_slider_get_value,
    .millis = api_millis, .epoch = api_epoch, .delay_ms = api_delay_ms,
    .app_dir = api_app_dir, .make_dirs = api_make_dirs, .pref_get = api_pref_get, .pref_set = api_pref_set,
    .beep = api_beep, .volume_get = api_volume_get, .led_set = api_led_set, .led_off = api_led_off,
    .imu_read = api_imu_read, .battery_read = api_battery_read, .net_online = api_net_online,
    .free_psram = api_free_psram, .free_sram = api_free_sram,
};

/* libc calls the loader's own table does not export (the apps use them) */
static const struct esp_elfsym s_extra_syms[] = {
    ESP_ELFSYM_EXPORT(snprintf), ESP_ELFSYM_EXPORT(fopen), ESP_ELFSYM_EXPORT(fclose), ESP_ELFSYM_EXPORT(fread),
    ESP_ELFSYM_EXPORT(fseek), ESP_ELFSYM_EXPORT(ftell), ESP_ELFSYM_EXPORT(remove), ESP_ELFSYM_EXPORT(memcmp),
    ESP_ELFSYM_EXPORT(strcmp), ESP_ELFSYM_EXPORT(strstr), ESP_ELFSYM_EXPORT(atoi), ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strncpy), ESP_ELFSYM_EXPORT(strcat),
    ESP_ELFSYM_END,
};

/* ---- host ---- */
static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = sz > 0 ? heap_caps_malloc((sz + 3) & ~3l, MALLOC_CAP_8BIT) : NULL;   /* the loader wants a 4-byte aligned image */
    if (!buf || fread(buf, 1, sz, f) != (size_t)sz) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = sz;
    return buf;
}

static void set_err(char *err, size_t len, const char *msg)
{
    if (err && len) {
        snprintf(err, len, "%s", msg);
    }
}

int phone_elf_host_open(const char *app_id, const char *elf_path, lv_obj_t *screen,
                        phone_elf_host_close_cb_t close_cb, void *user, char *err, size_t err_len)
{
    if (s_active) {
        set_err(err, err_len, "another ELF app is open");
        return -2;
    }
    if (!s_registered) {
        ESP_LOGI(TAG, "register extra symbols: %d", esp_elf_register_symbol(s_extra_syms));
        s_registered = true;
    }
    size_t n = 0;
    uint8_t *img = read_file(elf_path, &n);
    if (!img) {
        set_err(err, err_len, "cannot read the ELF file");
        return -1;
    }
    snprintf(s_id, sizeof(s_id), "%s", app_id);
    snprintf(s_data_dir, sizeof(s_data_dir), "%s/%s", PHONE_SD_APPDATA, app_id);
    uint32_t h = 2166136261u;
    for (const char *c = app_id; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    snprintf(s_pref_prefix, sizeof(s_pref_prefix), "e%03x_", (unsigned)(h & 0xFFF));

    esp_elf_init(&s_elf);
    int64_t t0 = esp_timer_get_time();
    int r = esp_elf_relocate(&s_elf, img, n);
    ESP_LOGI(TAG, "[%s] %u bytes, relocate %d, %lld us, base 0x%08x, free PSRAM %u, LVGL task stack min %u",
             s_id, (unsigned)n, r, (long long)(esp_timer_get_time() - t0), (unsigned)esp_elf_runtime_base(&s_elf),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(img);   /* the loader copied what it needs */
    if (r != 0) {
        esp_elf_deinit(&s_elf);
        set_err(err, err_len, "cannot load the ELF (symbol or format)");
        return -1;
    }

    s_root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, 12, 0);
    lv_obj_set_style_pad_row(s_root, 6, 0);
    lv_obj_set_style_text_color(s_root, lv_color_hex(0xFFFFFF), 0);
    s_close_cb = close_cb;
    s_close_user = user;
    s_active = true;

    char *argv[] = {(char *)&s_api};
    t0 = esp_timer_get_time();
    r = esp_elf_request(&s_elf, 0, 1, argv);
    ESP_LOGI(TAG, "[%s] entry returned %d, %lld us", s_id, r, (long long)(esp_timer_get_time() - t0));
    return 0;
}

void phone_elf_host_close(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    /* The objects go first, under the LVGL lock the caller holds: no callback of the app runs or can start after this. */
    if (s_root) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    phone_led_release(s_id);
    esp_elf_deinit(&s_elf);
    ESP_LOGI(TAG, "[%s] closed; free PSRAM %u, SRAM %u", s_id, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool phone_elf_host_is_open(void)
{
    return s_active;
}
