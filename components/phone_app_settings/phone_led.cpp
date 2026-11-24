/* Shared RGB LED service: requests by owner and priority, effects run by an esp_timer (see phone_led.h) */
#include <cmath>
#include <cstring>
#include <mutex>
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "phone_led.h"

namespace {

constexpr const char *TAG = "phone_led";
constexpr int MAX_REQUESTS = 8;
constexpr uint64_t TICK_US = 30 * 1000;

struct Request {
    char owner[24];
    phone_led_prio_t prio;
    phone_led_pattern_t pattern;
    uint32_t seq;           // newer requests win among equal priorities
    int64_t start_us;
    bool used;
};

const phone_led_effect_info_t EFFECTS[PHONE_LED_EFFECT_COUNT] = {
    {"Đứng yên", "Solid", true, false},
    {"Nháy", "Blink", true, true},
    {"Thở", "Breathe", true, true},
    {"Cầu vồng", "Rainbow", false, true},
};

std::mutex s_mtx;
Request s_req[MAX_REQUESTS];
uint32_t s_seq;
bool s_enabled = true;
int s_bright = 30;
bool s_ready;               // the LED driver is up
bool s_timer_on;
esp_timer_handle_t s_timer;
uint32_t s_written = 0xFFFFFFFF;    // last color sent to the LED

int winner()
{
    int w = -1;
    for (int i = 0; i < MAX_REQUESTS; i++) {
        if (!s_req[i].used) {
            continue;
        }
        if (w < 0 || s_req[i].prio > s_req[w].prio || (s_req[i].prio == s_req[w].prio && s_req[i].seq > s_req[w].seq)) {
            w = i;
        }
    }
    return w;
}

int find(const char *owner)
{
    for (int i = 0; i < MAX_REQUESTS; i++) {
        if (s_req[i].used && strcmp(s_req[i].owner, owner) == 0) {
            return i;
        }
    }
    return -1;
}

uint32_t hue_color(float h)
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

/* Color of a request at its age `t_ms`, before the brightness; `level` 0..1 scales it */
uint32_t effect_color(const phone_led_pattern_t &p, int64_t t_ms, float *level)
{
    const int64_t period = p.period_ms ? p.period_ms : 1000;
    *level = 1.0f;
    switch (p.effect) {
    case PHONE_LED_BLINK:
        *level = (t_ms % period) < p.on_ms ? 1.0f : 0.0f;
        return p.color;
    case PHONE_LED_BREATHE:
        *level = (1.0f - cosf(2.0f * static_cast<float>(M_PI) * (t_ms % period) / period)) / 2.0f;
        return p.color;
    case PHONE_LED_RAINBOW:
        return hue_color(360.0f * (t_ms % period) / period);
    default:
        return p.color;
    }
}

/* Gamma 2.2: the eye sees the low levels of a LED much brighter than their value */
/* One channel 0..255 (float): gamma 2.2 on the color, the scale applied after it */
float channel(uint32_t c, float scale)
{
    return powf((c & 0xFF) / 255.0f, 2.2f) * 255.0f * scale;
}

void write(uint32_t rgb_scaled_key, uint8_t r, uint8_t g, uint8_t b)
{
    if (rgb_scaled_key == s_written) {
        return;
    }
    s_written = rgb_scaled_key;
    bsp_led_rgb_set(r, g, b);
}

void timer_set(bool on)
{
    if (on == s_timer_on || !s_timer) {
        return;
    }
    s_timer_on = on;
    if (on) {
        esp_timer_start_periodic(s_timer, TICK_US);
    } else {
        esp_timer_stop(s_timer);
    }
}

/* Show the winning request now; with s_mtx held */
void update_locked()
{
    if (!s_ready) {
        return;
    }
    const int w = s_enabled ? winner() : -1;
    if (w < 0) {
        timer_set(false);
        write(0, 0, 0, 0);
        return;
    }
    const Request &r = s_req[w];
    float level;
    const uint32_t c = effect_color(r.pattern, (esp_timer_get_time() - r.start_us) / 1000, &level);
    /* brightness on a perceived scale (squared), and a lit LED never rounds down to black: at a low brightness
     * the strongest channel keeps 1 (the WS2812 has 256 steps, 1% of a squared scale is far below one) */
    const float b = s_bright / 100.0f;
    const float scale = level * b * b;
    float fr = channel(c >> 16, scale), fg = channel(c >> 8, scale), fb = channel(c, scale);
    const float top = fmaxf(fr, fmaxf(fg, fb));
    if (top > 0.0f && top < 1.0f) {
        fr /= top;
        fg /= top;
        fb /= top;
    }
    const uint8_t R = static_cast<uint8_t>(fr + 0.5f), G = static_cast<uint8_t>(fg + 0.5f), B = static_cast<uint8_t>(fb + 0.5f);
    write((static_cast<uint32_t>(R) << 16) | (G << 8) | B, R, G, B);
    timer_set(r.pattern.effect != PHONE_LED_SOLID);
}

void on_tick(void *)
{
    std::lock_guard<std::mutex> lock(s_mtx);
    update_locked();
}

/* The LED driver and the timer, made on the first request */
void ensure_ready_locked()
{
    if (s_ready) {
        return;
    }
    if (bsp_led_rgb_init() != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed");
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = on_tick,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "phone_led",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer create failed");
        return;
    }
    s_written = 0xFFFFFFFF;
    s_ready = true;
}

} // namespace

extern "C" void phone_led_request(const char *owner, phone_led_prio_t prio, const phone_led_pattern_t *pattern)
{
    if (!owner || !pattern) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mtx);
    ensure_ready_locked();
    int i = find(owner);
    for (int k = 0; i < 0 && k < MAX_REQUESTS; k++) {
        if (!s_req[k].used) {
            i = k;
        }
    }
    if (i < 0) {
        ESP_LOGW(TAG, "too many LED users, \"%s\" ignored", owner);
        return;
    }
    Request &r = s_req[i];
    strlcpy(r.owner, owner, sizeof(r.owner));
    r.prio = prio;
    r.pattern = *pattern;
    r.seq = ++s_seq;
    r.start_us = esp_timer_get_time();
    r.used = true;
    update_locked();
}

extern "C" void phone_led_release(const char *owner)
{
    if (!owner) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mtx);
    const int i = find(owner);
    if (i >= 0) {
        s_req[i].used = false;
        update_locked();
    }
}

extern "C" bool phone_led_is_shown(const char *owner)
{
    std::lock_guard<std::mutex> lock(s_mtx);
    const int w = winner();
    return owner && w >= 0 && strcmp(s_req[w].owner, owner) == 0;
}

extern "C" const char *phone_led_shown_owner(char *buf, int len)
{
    std::lock_guard<std::mutex> lock(s_mtx);
    const int w = winner();
    strlcpy(buf, w >= 0 ? s_req[w].owner : "", len);
    return buf;
}

extern "C" void phone_led_set_enabled(bool on)
{
    std::lock_guard<std::mutex> lock(s_mtx);
    s_enabled = on;
    update_locked();
}

extern "C" bool phone_led_enabled(void)
{
    return s_enabled;
}

extern "C" void phone_led_set_brightness(int percent)
{
    std::lock_guard<std::mutex> lock(s_mtx);
    s_bright = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    update_locked();
}

extern "C" int phone_led_brightness(void)
{
    return s_bright;
}

extern "C" const phone_led_effect_info_t *phone_led_effect_info(phone_led_effect_t effect)
{
    return effect >= 0 && effect < PHONE_LED_EFFECT_COUNT ? &EFFECTS[effect] : nullptr;
}
