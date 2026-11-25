/*
 * API v3 of the ELF apps of the phone.
 *
 * An app is an ELF loaded into PSRAM. Its entry is `int main(int argc, char *argv[])` with
 *     argv[0] = const phone_elf_api_t *   the table below
 *     argv[1] = phone_elf_ctx_t *         this app instance: the first argument of (almost) every call
 * Several apps run at the same time, each with its own copy of the image (so its own globals) and its own context.
 *
 * Rules
 *  - UI calls and the callbacks (events, timers) run on the LVGL task with the LVGL lock held: keep them short,
 *    do not block (delay_ms in a callback freezes the screen).
 *  - Return values: 0 or a positive value on success, a negative errno on failure (PHONE_ELF_E_*).
 *  - The only libc and system functions an app can link against are the ones in phone_elf_symbols.c (a chosen list: strings, memory,
 *    math, printf, sandboxed files). An ELF that needs another symbol is refused when it loads.
 *  - Files: fopen / remove / rename (libc names, sandboxed by the host) reach app_dir() with PERM_FILES, and pkg_dir() read only.
 *    Every other path is refused (NULL / error).
 *  - Calls of a group the manifest did not ask for (`"permissions"`) fail with PHONE_ELF_E_PERM and do nothing.
 *    Permissions are a declaration checked by the host, not a sandbox: an ELF can still call any libc or IDF
 *    function the loader exports. Install packages you trust.
 *  - Never remove or reorder a member: add at the end and raise PHONE_ELF_API_VERSION. `size` is the size of the
 *    table of the firmware: an app that uses a member added later checks `size` first (PHONE_ELF_HAS).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHONE_ELF_API_VERSION 3

typedef struct phone_elf_ctx phone_elf_ctx_t;
typedef void (*phone_elf_event_cb_t)(void *event);
typedef void (*phone_elf_timer_cb_t)(void *user);

/* errors (negative errno) */
#define PHONE_ELF_E_PERM    (-1)    /* the manifest does not ask for this permission */
#define PHONE_ELF_E_IO      (-5)
#define PHONE_ELF_E_NOMEM   (-12)
#define PHONE_ELF_E_ARG     (-22)

/* permissions: manifest.json "permissions": ["led", "audio", "imu", "battery", "net", "files"] */
#define PHONE_ELF_PERM_LED      (1u << 0)   /* led_set, led_off */
#define PHONE_ELF_PERM_AUDIO    (1u << 1)   /* beep, volume_get */
#define PHONE_ELF_PERM_IMU      (1u << 2)   /* imu_read */
#define PHONE_ELF_PERM_BATTERY  (1u << 3)   /* battery_read */
#define PHONE_ELF_PERM_NET      (1u << 4)   /* net_online */
#define PHONE_ELF_PERM_FILES    (1u << 5)   /* make_dirs (the folder of the app) */

enum { PHONE_ELF_EV_CLICKED = 0, PHONE_ELF_EV_VALUE_CHANGED = 1, PHONE_ELF_EV_LONG_PRESSED = 2 };
enum { PHONE_ELF_ALIGN_CENTER = 0, PHONE_ELF_ALIGN_TOP_MID = 1, PHONE_ELF_ALIGN_BOTTOM_MID = 2 };
enum { PHONE_ELF_LED_SOLID = 0, PHONE_ELF_LED_BLINK = 1, PHONE_ELF_LED_BREATHE = 2, PHONE_ELF_LED_RAINBOW = 3 };

typedef struct {
    int16_t gyro[3];
    int16_t accel[3];
} phone_elf_imu_t;

typedef struct {
    int percent;        /* -1 without battery */
    int voltage_mv;
    bool charging_usb;  /* USB supply present */
} phone_elf_battery_t;

typedef struct {
    uint32_t size;      /* sizeof of this table in the firmware */
    uint32_t version;   /* PHONE_ELF_API_VERSION */

    /* core */
    void (*log)(phone_elf_ctx_t *ctx, const char *text);
    void (*close)(phone_elf_ctx_t *ctx);                                    /* ask the host to close this app (after the callback returns) */
    const char *(*tr)(phone_elf_ctx_t *ctx, const char *vi, const char *en);/* text in the language of the phone */
    uint32_t (*permissions)(phone_elf_ctx_t *ctx);                          /* PHONE_ELF_PERM_* granted to this app */

    /* UI: LVGL objects are opaque pointers; everything made under root() is deleted when the app closes */
    void *(*root)(phone_elf_ctx_t *ctx);                                    /* full-screen container of the app */
    void *(*obj_create)(phone_elf_ctx_t *ctx, void *parent);
    void *(*label_create)(phone_elf_ctx_t *ctx, void *parent);
    void *(*button_create)(phone_elf_ctx_t *ctx, void *parent);
    void *(*slider_create)(phone_elf_ctx_t *ctx, void *parent);
    void *(*switch_create)(phone_elf_ctx_t *ctx, void *parent);
    void (*obj_delete)(phone_elf_ctx_t *ctx, void *obj);
    void (*label_set_text)(phone_elf_ctx_t *ctx, void *label, const char *text);
    void (*obj_set_size)(phone_elf_ctx_t *ctx, void *obj, int w, int h);    /* -1 = fit the content, another negative value = percent of the parent (-100 = 100%) */
    void (*obj_set_pos)(phone_elf_ctx_t *ctx, void *obj, int x, int y);
    void (*obj_align)(phone_elf_ctx_t *ctx, void *obj, int align, int x, int y);
    void (*obj_set_bg_color)(phone_elf_ctx_t *ctx, void *obj, uint32_t rgb);
    void (*obj_set_text_color)(phone_elf_ctx_t *ctx, void *obj, uint32_t rgb);
    void (*obj_set_radius)(phone_elf_ctx_t *ctx, void *obj, int radius);
    void (*obj_set_hidden)(phone_elf_ctx_t *ctx, void *obj, bool hidden);
    void (*obj_set_flex_column)(phone_elf_ctx_t *ctx, void *obj);
    void (*obj_set_flex_row)(phone_elf_ctx_t *ctx, void *obj);
    void (*obj_add_event_cb)(phone_elf_ctx_t *ctx, void *obj, phone_elf_event_cb_t cb, int event, void *user_data);
    void *(*event_target)(void *event);                                     /* inside an event callback */
    void *(*event_user_data)(void *event);
    void (*slider_set_range)(phone_elf_ctx_t *ctx, void *slider, int min, int max);
    void (*slider_set_value)(phone_elf_ctx_t *ctx, void *slider, int value);
    int (*slider_get_value)(phone_elf_ctx_t *ctx, void *slider);
    void (*switch_set_state)(phone_elf_ctx_t *ctx, void *sw, bool on);
    bool (*switch_get_state)(phone_elf_ctx_t *ctx, void *sw);

    /* timers: run on the LVGL task; the host deletes the ones left when the app closes (max 8 per app) */
    void *(*timer_create)(phone_elf_ctx_t *ctx, uint32_t period_ms, phone_elf_timer_cb_t cb, void *user_data);   /* NULL when it fails */
    void (*timer_delete)(phone_elf_ctx_t *ctx, void *timer);

    /* time */
    uint32_t (*millis)(phone_elf_ctx_t *ctx);
    int64_t (*epoch)(phone_elf_ctx_t *ctx);                                 /* seconds since 1970, 0 when the time is not set */
    void (*delay_ms)(phone_elf_ctx_t *ctx, uint32_t ms);                    /* blocks the LVGL task: for start-up work only */

    /* files: the data folder of the app is /sdcard/appdata/<id>; libc fopen works on it */
    const char *(*app_dir)(phone_elf_ctx_t *ctx);
    int (*make_dirs)(phone_elf_ctx_t *ctx, const char *file_path);          /* create the folders above a file inside app_dir(); PERM_FILES */

    /* settings of the app: 32-bit values, key up to 9 characters, kept apart from the system and from other apps */
    int32_t (*pref_get)(phone_elf_ctx_t *ctx, const char *key, int32_t def);
    int (*pref_set)(phone_elf_ctx_t *ctx, const char *key, int32_t value);

    /* hardware and services */
    int (*beep)(phone_elf_ctx_t *ctx);                                      /* PERM_AUDIO */
    int (*volume_get)(phone_elf_ctx_t *ctx);                                /* PERM_AUDIO: percent, or a negative error */
    int (*led_set)(phone_elf_ctx_t *ctx, uint32_t rgb, int effect, int period_ms);   /* PERM_LED: 0xRRGGBB, PHONE_ELF_LED_* */
    int (*led_off)(phone_elf_ctx_t *ctx);                                   /* PERM_LED */
    int (*imu_read)(phone_elf_ctx_t *ctx, phone_elf_imu_t *out);            /* PERM_IMU */
    int (*battery_read)(phone_elf_ctx_t *ctx, phone_elf_battery_t *out);    /* PERM_BATTERY */
    int (*net_online)(phone_elf_ctx_t *ctx);                                /* PERM_NET: 1 online, 0 offline, or a negative error */
    uint32_t (*free_psram)(phone_elf_ctx_t *ctx);
    uint32_t (*free_sram)(phone_elf_ctx_t *ctx);

    /* files: the folder of the installed package (/sdcard/userapps/<id>), read only: its res/, icon, data files */
    const char *(*pkg_dir)(phone_elf_ctx_t *ctx);
} phone_elf_api_t;

/* in the app: is a member there in the firmware that runs it? */
#define PHONE_ELF_HAS(api, member) ((api)->size >= offsetof(phone_elf_api_t, member) + sizeof((api)->member))
