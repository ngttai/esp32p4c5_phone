/*
 * The RGB LED of the board (one WS2812B), shared by the apps. Each user ("owner", a string literal such as
 * "Recorder") holds at most one request; the highest priority wins, the newest among equals. Releasing the
 * request shown brings back the one under it. The service runs the effects itself (an esp_timer while an
 * animated effect shows, nothing at all when no request is left) and applies the master switch and the
 * brightness, so an app never drives the LED directly. Callable from any task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHONE_LED_SOLID,
    PHONE_LED_BLINK,        /*!< on for on_ms at the start of each period */
    PHONE_LED_BREATHE,      /*!< fades in and out over the period */
    PHONE_LED_RAINBOW,      /*!< goes round the hues over the period (the color is not used) */
    PHONE_LED_EFFECT_COUNT,
} phone_led_effect_t;

typedef enum {
    PHONE_LED_AMBIENT = 0,  /*!< decoration: the LED RGB app */
    PHONE_LED_STATUS,       /*!< a lasting state */
    PHONE_LED_ALERT,        /*!< must be seen at once: recording */
} phone_led_prio_t;

typedef struct {
    phone_led_effect_t effect;
    uint32_t color;         /*!< 0xRRGGBB */
    uint16_t period_ms;
    uint16_t on_ms;         /*!< PHONE_LED_BLINK only */
} phone_led_pattern_t;

typedef struct {
    const char *name_vi;
    const char *name_en;
    bool uses_color;
    bool uses_period;
} phone_led_effect_info_t;

/** Set (or replace) the request of `owner`; the effect restarts. */
void phone_led_request(const char *owner, phone_led_prio_t prio, const phone_led_pattern_t *pattern);
void phone_led_release(const char *owner);
/** The request of `owner` is the one the LED shows (false while another owner's request wins). */
bool phone_led_is_shown(const char *owner);
/** Owner of the request shown, copied into buf ("" when none); returns buf. */
const char *phone_led_shown_owner(char *buf, int len);

/** Master switch (on at boot, not stored): off keeps the requests but turns the LED off. */
void phone_led_set_enabled(bool on);
bool phone_led_enabled(void);
/** Brightness of every effect, 0..100 (30 at boot, not stored). */
void phone_led_set_brightness(int percent);
int phone_led_brightness(void);

const phone_led_effect_info_t *phone_led_effect_info(phone_led_effect_t effect);

#ifdef __cplusplus
}
#endif
