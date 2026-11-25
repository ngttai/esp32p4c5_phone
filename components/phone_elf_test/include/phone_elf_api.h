/*
 * API the firmware hands to an ELF app (experiment). argv[0] of the ELF entry points to this struct.
 * LVGL objects are opaque pointers. LVGL callbacks and the entry run with the LVGL lock held.
 * Never remove or reorder a member: add at the end and raise PHONE_ELF_API_VERSION.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PHONE_ELF_API_VERSION 2

typedef void (*phone_elf_event_cb_t)(void *event);

enum { PHONE_ELF_EV_CLICKED = 0, PHONE_ELF_EV_VALUE_CHANGED = 1 };
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
    uint32_t version;

    /* core */
    void (*log)(const char *text);
    void (*close)(void);                                    /* ask the host to close the app (after the callback returns) */
    const char *(*tr)(const char *vi, const char *en);      /* text in the language of the phone */

    /* LVGL (all under the LVGL lock) */
    void *(*root)(void);                                    /* full-screen container of the app */
    void *(*obj_create)(void *parent);
    void *(*label_create)(void *parent);
    void *(*button_create)(void *parent);
    void *(*slider_create)(void *parent);
    void (*label_set_text)(void *label, const char *text);
    void (*obj_set_size)(void *obj, int w, int h);          /* -1 = fit the content, another negative value = percent of the parent (-100 = 100%) */
    void (*obj_align)(void *obj, int align, int x, int y);
    void (*obj_set_bg_color)(void *obj, uint32_t rgb);
    void (*obj_set_text_color)(void *obj, uint32_t rgb);
    void (*obj_set_flex_column)(void *obj);
    void (*obj_set_flex_row)(void *obj);
    void (*obj_add_event_cb)(void *obj, phone_elf_event_cb_t cb, int event, void *user_data);
    void *(*event_target)(void *event);
    void *(*event_user_data)(void *event);
    void (*slider_set_range)(void *slider, int min, int max);
    void (*slider_set_value)(void *slider, int value);
    int (*slider_get_value)(void *slider);

    /* time */
    uint32_t (*millis)(void);
    int64_t (*epoch)(void);                                 /* seconds since 1970, 0 when the time is not set */
    void (*delay_ms)(uint32_t ms);

    /* files: only under the folder of the app */
    const char *(*app_dir)(void);                           /* "/sdcard/appdata/<app>" */
    int (*make_dirs)(const char *file_path);                /* create the folders above a file; 0 when they exist */

    /* settings of the app (NVS, key up to 10 characters, kept apart from the system settings; a value is 0..255, the system stores one byte) */
    int (*pref_get)(const char *key, int def);
    void (*pref_set)(const char *key, int value);

    /* hardware and services */
    void (*beep)(void);
    int (*volume_get)(void);
    void (*led_set)(uint32_t rgb, int effect, int period_ms);   /* 0xRRGGBB, PHONE_ELF_LED_* */
    void (*led_off)(void);
    int (*imu_read)(phone_elf_imu_t *out);                  /* 0 on success */
    int (*battery_read)(phone_elf_battery_t *out);          /* 0 on success */
    bool (*net_online)(void);
    uint32_t (*free_psram)(void);
    uint32_t (*free_sram)(void);
} phone_elf_api_t;
