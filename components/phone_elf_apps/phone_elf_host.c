#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
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

#define MAGIC       0x454C4658u
#define MAX_TIMERS  8

static const char *TAG = "ELFHOST";

typedef struct {
    lv_timer_t *timer;
    phone_elf_timer_cb_t cb;
    void *user;
} elf_timer_t;

struct phone_elf_inst {
    uint32_t magic;
    bool closing;
    esp_elf_t elf;
    lv_obj_t *root;
    unsigned perms;
    char id[40];
    char data_dir[96];
    char pref_prefix[8];                    /* "e" + 3 hex digits of the app id + "_" */
    phone_elf_host_close_cb_t close_cb;
    void *close_user;
    elf_timer_t timers[MAX_TIMERS];
};

static phone_elf_inst_t *s_insts[PHONE_ELF_HOST_MAX_APPS];
static bool s_registered;

/* ---- the context of a call ---- */
static phone_elf_inst_t *get(phone_elf_ctx_t *ctx)
{
    for (int i = 0; i < PHONE_ELF_HOST_MAX_APPS; i++) {
        if (s_insts[i] && (void *)s_insts[i] == (void *)ctx && s_insts[i]->magic == MAGIC && !s_insts[i]->closing) {
            return s_insts[i];
        }
    }
    return NULL;
}

/* the context, when the call needs `perm` (0 = no permission): NULL when it is not valid or not granted (logged) */
static phone_elf_inst_t *guard(phone_elf_ctx_t *ctx, unsigned perm, const char *what)
{
    phone_elf_inst_t *in = get(ctx);
    if (!in) {
        return NULL;
    }
    if (perm && !(in->perms & perm)) {
        ESP_LOGW(TAG, "[%s] %s: permission not in the manifest", in->id, what);
        return NULL;
    }
    return in;
}
#define G(perm) guard(ctx, perm, __func__)

/* ---- API implementation ---- */
static void api_log(phone_elf_ctx_t *ctx, const char *text) { phone_elf_inst_t *in = G(0); if (in) ESP_LOGI(TAG, "[%s] %s", in->id, text); }
static void close_async(void *arg)
{
    phone_elf_inst_t *in = get((phone_elf_ctx_t *)arg);     /* still there? */
    if (in && in->close_cb) in->close_cb(in->close_user);
}
static void api_close(phone_elf_ctx_t *ctx) { if (G(0)) lv_async_call(close_async, ctx); }   /* after the callback of the app has returned */
static const char *api_tr(phone_elf_ctx_t *ctx, const char *vi, const char *en) { return G(0) ? phone_tr(vi, en) : en; }
static uint32_t api_permissions(phone_elf_ctx_t *ctx) { phone_elf_inst_t *in = G(0); return in ? in->perms : 0; }

static void *api_root(phone_elf_ctx_t *ctx) { phone_elf_inst_t *in = G(0); return in ? in->root : NULL; }
static void *api_obj_create(phone_elf_ctx_t *ctx, void *p)
{
    if (!G(0)) return NULL;
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    return o;
}
static void *api_label_create(phone_elf_ctx_t *ctx, void *p)
{
    if (!G(0)) return NULL;
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}
static void *api_button_create(phone_elf_ctx_t *ctx, void *p) { return G(0) ? lv_button_create(p) : NULL; }
static void *api_slider_create(phone_elf_ctx_t *ctx, void *p) { return G(0) ? lv_slider_create(p) : NULL; }
static void *api_switch_create(phone_elf_ctx_t *ctx, void *p) { return G(0) ? lv_switch_create(p) : NULL; }
static void api_obj_delete(phone_elf_ctx_t *ctx, void *o) { if (G(0) && o && o != get(ctx)->root) lv_obj_delete(o); }
static void api_label_set_text(phone_elf_ctx_t *ctx, void *l, const char *t) { if (G(0) && l && t) lv_label_set_text(l, t); }
static int32_t dim(int v) { return v == -1 ? LV_SIZE_CONTENT : v < 0 ? lv_pct(-v) : v; }
static void api_obj_set_size(phone_elf_ctx_t *ctx, void *o, int w, int h) { if (G(0) && o) lv_obj_set_size(o, dim(w), dim(h)); }
static void api_obj_set_pos(phone_elf_ctx_t *ctx, void *o, int x, int y) { if (G(0) && o) lv_obj_set_pos(o, x, y); }
static void api_obj_align(phone_elf_ctx_t *ctx, void *o, int a, int x, int y)
{
    if (!G(0) || !o) return;
    lv_align_t al = a == PHONE_ELF_ALIGN_TOP_MID ? LV_ALIGN_TOP_MID : a == PHONE_ELF_ALIGN_BOTTOM_MID ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_CENTER;
    lv_obj_align(o, al, x, y);
}
static void api_obj_set_bg_color(phone_elf_ctx_t *ctx, void *o, uint32_t rgb) { if (G(0) && o) { lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0); } }
static void api_obj_set_text_color(phone_elf_ctx_t *ctx, void *o, uint32_t rgb) { if (G(0) && o) lv_obj_set_style_text_color(o, lv_color_hex(rgb), 0); }
static void api_obj_set_radius(phone_elf_ctx_t *ctx, void *o, int r) { if (G(0) && o) lv_obj_set_style_radius(o, r, 0); }
static void api_obj_set_hidden(phone_elf_ctx_t *ctx, void *o, bool hidden)
{
    if (!G(0) || !o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static void api_obj_set_flex_column(phone_elf_ctx_t *ctx, void *o) { if (G(0) && o) lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN); }
static void api_obj_set_flex_row(phone_elf_ctx_t *ctx, void *o)
{
    if (!G(0) || !o) return;
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}
static void api_obj_add_event_cb(phone_elf_ctx_t *ctx, void *o, phone_elf_event_cb_t cb, int ev, void *ud)
{
    if (!G(0) || !o || !cb) return;
    lv_event_code_t code = ev == PHONE_ELF_EV_VALUE_CHANGED ? LV_EVENT_VALUE_CHANGED : ev == PHONE_ELF_EV_LONG_PRESSED ? LV_EVENT_LONG_PRESSED : LV_EVENT_CLICKED;
    lv_obj_add_event_cb(o, (lv_event_cb_t)cb, code, ud);
}
static void *api_event_target(void *e) { return lv_event_get_target(e); }
static void *api_event_user_data(void *e) { return lv_event_get_user_data(e); }
static void api_slider_set_range(phone_elf_ctx_t *ctx, void *s, int a, int b) { if (G(0) && s) lv_slider_set_range(s, a, b); }
static void api_slider_set_value(phone_elf_ctx_t *ctx, void *s, int v) { if (G(0) && s) lv_slider_set_value(s, v, LV_ANIM_OFF); }
static int api_slider_get_value(phone_elf_ctx_t *ctx, void *s) { return (G(0) && s) ? lv_slider_get_value(s) : 0; }
static void api_switch_set_state(phone_elf_ctx_t *ctx, void *sw, bool on)
{
    if (!G(0) || !sw) return;
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED); else lv_obj_remove_state(sw, LV_STATE_CHECKED);
}
static bool api_switch_get_state(phone_elf_ctx_t *ctx, void *sw) { return G(0) && sw && lv_obj_has_state(sw, LV_STATE_CHECKED); }

static void timer_adapter(lv_timer_t *t)
{
    elf_timer_t *m = lv_timer_get_user_data(t);
    if (m && m->cb) m->cb(m->user);
}
static void *api_timer_create(phone_elf_ctx_t *ctx, uint32_t period_ms, phone_elf_timer_cb_t cb, void *user)
{
    phone_elf_inst_t *in = G(0);
    if (!in || !cb || period_ms == 0) return NULL;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!in->timers[i].timer) {
            in->timers[i].cb = cb;
            in->timers[i].user = user;
            in->timers[i].timer = lv_timer_create(timer_adapter, period_ms, &in->timers[i]);
            return in->timers[i].timer ? &in->timers[i] : NULL;
        }
    }
    ESP_LOGW(TAG, "[%s] too many timers", in->id);
    return NULL;
}
static void drop_timer(elf_timer_t *m)
{
    if (m->timer) lv_timer_delete(m->timer);
    memset(m, 0, sizeof(*m));
}
static void api_timer_delete(phone_elf_ctx_t *ctx, void *timer)
{
    phone_elf_inst_t *in = G(0);
    if (!in) return;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timer == &in->timers[i] && in->timers[i].timer) drop_timer(&in->timers[i]);
    }
}

static uint32_t api_millis(phone_elf_ctx_t *ctx) { (void)ctx; return (uint32_t)(esp_timer_get_time() / 1000); }
static int64_t api_epoch(phone_elf_ctx_t *ctx) { (void)ctx; time_t t = time(NULL); return t > 1700000000 ? (int64_t)t : 0; }
static void api_delay_ms(phone_elf_ctx_t *ctx, uint32_t ms) { (void)ctx; vTaskDelay(pdMS_TO_TICKS(ms)); }

static const char *api_app_dir(phone_elf_ctx_t *ctx) { phone_elf_inst_t *in = G(0); return in ? in->data_dir : ""; }
static int api_make_dirs(phone_elf_ctx_t *ctx, const char *path)
{
    phone_elf_inst_t *in = G(PHONE_ELF_PERM_FILES);
    if (!in) return get(ctx) ? PHONE_ELF_E_PERM : PHONE_ELF_E_ARG;
    size_t n = strlen(in->data_dir);
    if (!path || strncmp(path, in->data_dir, n) != 0 || path[n] != '/' || strstr(path, "..")) {
        return PHONE_ELF_E_ARG;     /* only below the data folder of the app */
    }
    return phone_sd_make_dirs(path) == 0 ? 0 : PHONE_ELF_E_IO;
}

static int32_t api_pref_get(phone_elf_ctx_t *ctx, const char *key, int32_t def)
{
    phone_elf_inst_t *in = G(0);
    if (!in || !key) return def;
    char k[24];
    snprintf(k, sizeof(k), "%s%.9s", in->pref_prefix, key);
    nvs_handle_t h;
    int32_t v = def;
    if (nvs_open("elfapps", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, k, &v) != ESP_OK) v = def;
        nvs_close(h);
    }
    return v;
}
static int api_pref_set(phone_elf_ctx_t *ctx, const char *key, int32_t value)
{
    phone_elf_inst_t *in = G(0);
    if (!in || !key) return PHONE_ELF_E_ARG;
    char k[24];
    snprintf(k, sizeof(k), "%s%.9s", in->pref_prefix, key);
    nvs_handle_t h;
    if (nvs_open("elfapps", NVS_READWRITE, &h) != ESP_OK) return PHONE_ELF_E_IO;
    esp_err_t e = nvs_set_i32(h, k, value);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : PHONE_ELF_E_IO;
}

static int api_beep(phone_elf_ctx_t *ctx) { if (!G(PHONE_ELF_PERM_AUDIO)) return PHONE_ELF_E_PERM; phone_audio_beep(); return 0; }
static int api_volume_get(phone_elf_ctx_t *ctx) { return G(PHONE_ELF_PERM_AUDIO) ? phone_audio_get_volume() : PHONE_ELF_E_PERM; }
static int api_led_set(phone_elf_ctx_t *ctx, uint32_t rgb, int effect, int period_ms)
{
    phone_elf_inst_t *in = G(PHONE_ELF_PERM_LED);
    if (!in) return PHONE_ELF_E_PERM;
    phone_led_pattern_t p = {
        .effect = effect == PHONE_ELF_LED_BLINK ? PHONE_LED_BLINK : effect == PHONE_ELF_LED_BREATHE ? PHONE_LED_BREATHE :
                  effect == PHONE_ELF_LED_RAINBOW ? PHONE_LED_RAINBOW : PHONE_LED_SOLID,
        .color = rgb, .period_ms = period_ms > 0 ? period_ms : 1000, .on_ms = 300,
    };
    phone_led_request(in->id, PHONE_LED_AMBIENT, &p);
    return 0;
}
static int api_led_off(phone_elf_ctx_t *ctx)
{
    phone_elf_inst_t *in = G(PHONE_ELF_PERM_LED);
    if (!in) return PHONE_ELF_E_PERM;
    phone_led_release(in->id);
    return 0;
}
static int api_imu_read(phone_elf_ctx_t *ctx, phone_elf_imu_t *out)
{
    if (!G(PHONE_ELF_PERM_IMU)) return PHONE_ELF_E_PERM;
    if (!out) return PHONE_ELF_E_ARG;
    static bool inited;
    if (!inited) { bsp_imu_init(); inited = true; vTaskDelay(pdMS_TO_TICKS(50)); }   /* the first sample comes after the first period */
    bsp_imu_raw_t raw = {0};
    for (int i = 0; i < 5; i++) {
        if (bsp_imu_read(&raw) != ESP_OK) return PHONE_ELF_E_IO;
        if (raw.accel[0] || raw.accel[1] || raw.accel[2]) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    memcpy(out->gyro, raw.gyro, sizeof(out->gyro));
    memcpy(out->accel, raw.accel, sizeof(out->accel));
    return 0;
}
static int api_battery_read(phone_elf_ctx_t *ctx, phone_elf_battery_t *out)
{
    if (!G(PHONE_ELF_PERM_BATTERY)) return PHONE_ELF_E_PERM;
    if (!out) return PHONE_ELF_E_ARG;
    bsp_battery_info_t b;
    if (!phone_power_get_battery(&b)) return PHONE_ELF_E_IO;
    out->percent = b.percent; out->voltage_mv = b.voltage_mv; out->charging_usb = b.vbus_present;
    return 0;
}
static int api_net_online(phone_elf_ctx_t *ctx) { return G(PHONE_ELF_PERM_NET) ? (phone_net_online() ? 1 : 0) : PHONE_ELF_E_PERM; }
static uint32_t api_free_psram(phone_elf_ctx_t *ctx) { (void)ctx; return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
static uint32_t api_free_sram(phone_elf_ctx_t *ctx) { (void)ctx; return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

static const phone_elf_api_t s_api = {
    .size = sizeof(phone_elf_api_t), .version = PHONE_ELF_API_VERSION,
    .log = api_log, .close = api_close, .tr = api_tr, .permissions = api_permissions,
    .root = api_root, .obj_create = api_obj_create, .label_create = api_label_create, .button_create = api_button_create,
    .slider_create = api_slider_create, .switch_create = api_switch_create, .obj_delete = api_obj_delete,
    .label_set_text = api_label_set_text, .obj_set_size = api_obj_set_size, .obj_set_pos = api_obj_set_pos,
    .obj_align = api_obj_align, .obj_set_bg_color = api_obj_set_bg_color, .obj_set_text_color = api_obj_set_text_color,
    .obj_set_radius = api_obj_set_radius, .obj_set_hidden = api_obj_set_hidden,
    .obj_set_flex_column = api_obj_set_flex_column, .obj_set_flex_row = api_obj_set_flex_row,
    .obj_add_event_cb = api_obj_add_event_cb, .event_target = api_event_target, .event_user_data = api_event_user_data,
    .slider_set_range = api_slider_set_range, .slider_set_value = api_slider_set_value, .slider_get_value = api_slider_get_value,
    .switch_set_state = api_switch_set_state, .switch_get_state = api_switch_get_state,
    .timer_create = api_timer_create, .timer_delete = api_timer_delete,
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

unsigned phone_elf_host_parse_permission(const char *name)
{
    static const struct { const char *name; unsigned bit; } map[] = {
        {"led", PHONE_ELF_PERM_LED}, {"audio", PHONE_ELF_PERM_AUDIO}, {"imu", PHONE_ELF_PERM_IMU},
        {"battery", PHONE_ELF_PERM_BATTERY}, {"net", PHONE_ELF_PERM_NET}, {"files", PHONE_ELF_PERM_FILES},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (name && strcmp(name, map[i].name) == 0) return map[i].bit;
    }
    return 0;
}

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

phone_elf_inst_t *phone_elf_host_open(const char *app_id, const char *elf_path, unsigned perms, lv_obj_t *screen,
                                      phone_elf_host_close_cb_t close_cb, void *user, char *err, size_t err_len)
{
    int slot = -1;
    for (int i = 0; i < PHONE_ELF_HOST_MAX_APPS; i++) {
        if (!s_insts[i]) { slot = i; break; }
    }
    if (slot < 0) {
        set_err(err, err_len, "too many ELF apps are open");
        return NULL;
    }
    if (!s_registered) {
        ESP_LOGI(TAG, "register extra symbols: %d", esp_elf_register_symbol(s_extra_syms));
        s_registered = true;
    }
    size_t n = 0;
    uint8_t *img = read_file(elf_path, &n);
    if (!img) {
        set_err(err, err_len, "cannot read the ELF file");
        return NULL;
    }
    phone_elf_inst_t *in = heap_caps_calloc(1, sizeof(*in), MALLOC_CAP_8BIT);
    if (!in) {
        free(img);
        set_err(err, err_len, "out of memory");
        return NULL;
    }
    in->magic = MAGIC;
    in->perms = perms;
    snprintf(in->id, sizeof(in->id), "%s", app_id);
    snprintf(in->data_dir, sizeof(in->data_dir), "%s/%s", PHONE_SD_APPDATA, app_id);
    uint32_t h = 2166136261u;
    for (const char *c = app_id; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    snprintf(in->pref_prefix, sizeof(in->pref_prefix), "e%03x_", (unsigned)(h & 0xFFF));

    esp_elf_init(&in->elf);
    int64_t t0 = esp_timer_get_time();
    int r = esp_elf_relocate(&in->elf, img, n);
    ESP_LOGI(TAG, "[%s] %u bytes, relocate %d, %lld us, base 0x%08x, perms 0x%x, free PSRAM %u, LVGL task stack min %u",
             in->id, (unsigned)n, r, (long long)(esp_timer_get_time() - t0), (unsigned)esp_elf_runtime_base(&in->elf), perms,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(img);   /* the loader copied what it needs */
    if (r != 0) {
        esp_elf_deinit(&in->elf);
        free(in);
        set_err(err, err_len, "cannot load the ELF (symbol or format)");
        return NULL;
    }

    in->root = lv_obj_create(screen);
    lv_obj_remove_style_all(in->root);
    lv_obj_set_size(in->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(in->root, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(in->root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(in->root, 12, 0);
    lv_obj_set_style_pad_row(in->root, 6, 0);
    lv_obj_set_style_text_color(in->root, lv_color_hex(0xFFFFFF), 0);
    in->close_cb = close_cb;
    in->close_user = user;
    s_insts[slot] = in;

    char *argv[] = {(char *)&s_api, (char *)in};
    t0 = esp_timer_get_time();
    r = esp_elf_request(&in->elf, 0, 2, argv);
    ESP_LOGI(TAG, "[%s] entry returned %d, %lld us", in->id, r, (long long)(esp_timer_get_time() - t0));
    return in;
}

void phone_elf_host_close(phone_elf_inst_t *in)
{
    if (!in || !get((phone_elf_ctx_t *)in)) {
        return;
    }
    in->closing = true;     /* from now on every call of the app is refused */
    /* Timers and objects go first, under the LVGL lock the caller holds: no callback of the app runs or can start after this. */
    for (int i = 0; i < MAX_TIMERS; i++) {
        drop_timer(&in->timers[i]);
    }
    if (in->root) {
        lv_obj_delete(in->root);
        in->root = NULL;
    }
    phone_led_release(in->id);
    esp_elf_deinit(&in->elf);
    for (int i = 0; i < PHONE_ELF_HOST_MAX_APPS; i++) {
        if (s_insts[i] == in) s_insts[i] = NULL;
    }
    ESP_LOGI(TAG, "[%s] closed; free PSRAM %u, SRAM %u", in->id, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    in->magic = 0;
    heap_caps_free(in);
}

int phone_elf_host_count(void)
{
    int n = 0;
    for (int i = 0; i < PHONE_ELF_HOST_MAX_APPS; i++) {
        if (s_insts[i]) n++;
    }
    return n;
}
