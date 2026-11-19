/*
 * Weather app: current conditions and a 7-day forecast for a city picked from a list.
 *
 * Data comes from the open-meteo.com forecast API (HTTPS, no key). The request runs in a short
 * task that never touches the UI: it fills a result that the LVGL timer picks up.
 * The weather icons are drawn with plain LVGL objects, so there is no image or icon font.
 */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "WEATHER"
#include "esp_lib_utils.h"
#include "phone_ui.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_net.h"
#include "phone_app_weather.hpp"

#define APP_NAME "Thời tiết"

extern "C" const lv_image_dsc_t phone_weather_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_weather";

constexpr int DAYS = 7;
constexpr int REFRESH_S = 15 * 60;          // data older than this is fetched again when the app is shown
constexpr size_t BODY_MAX = 8192;

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_CARD = 0x1E252C;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0x2F80ED;
constexpr uint32_t COLOR_SUN = 0xFFC83D;
constexpr uint32_t COLOR_MOON = 0xDCE3F0;
constexpr uint32_t COLOR_CLOUD = 0xE8EDF3;
constexpr uint32_t COLOR_CLOUD_DARK = 0x9AA5B1;
constexpr uint32_t COLOR_RAIN = 0x4DA3FF;

struct City {
    const char *name;
    const char *lat;        // as text: the request URL is built without float formatting
    const char *lon;
};

const City CITIES[] = {
    {"Hà Nội", "21.0285", "105.8542"},
    {"TP. Hồ Chí Minh", "10.8231", "106.6297"},
    {"Đà Nẵng", "16.0544", "108.2022"},
    {"Hải Phòng", "20.8449", "106.6881"},
    {"Cần Thơ", "10.0452", "105.7469"},
    {"Huế", "16.4637", "107.5909"},
    {"Nha Trang", "12.2388", "109.1967"},
    {"Đà Lạt", "11.9404", "108.4583"},
    {"Vũng Tàu", "10.4114", "107.1362"},
    {"Quy Nhơn", "13.7830", "109.2197"},
    {"Buôn Ma Thuột", "12.6667", "108.0500"},
    {"Vinh", "18.6796", "105.6813"},
    {"Hạ Long", "20.9712", "107.0448"},
    {"Phú Quốc", "10.2899", "103.9840"},
};
constexpr int CITY_COUNT = sizeof(CITIES) / sizeof(CITIES[0]);
constexpr int CITY_DEFAULT = 1;

enum Kind { K_CLEAR, K_PARTLY, K_CLOUDY, K_FOG, K_RAIN, K_STORM, K_SNOW };

struct Day {
    char date[11];          // YYYY-MM-DD
    int code;
    int tmin, tmax;
    int rain;               // probability of precipitation, %
};

struct Weather {
    int city;
    int temp, feels, humidity, wind, code;
    bool is_day;
    int days;
    Day day[DAYS];
    time_t fetched;
};

enum Fetch { F_IDLE, F_BUSY, F_DONE, F_FAILED };

lv_obj_t *s_root;
lv_obj_t *s_city_btn;
lv_obj_t *s_updated;
lv_obj_t *s_content;
lv_obj_t *s_cities;         // city picker page
int s_city;
Weather s_wx;               // shown data
bool s_have_wx;
Weather s_new;              // written by the fetch task while F_BUSY
std::atomic<int> s_fetch{F_IDLE};
int s_fetch_city;

Kind code_kind(int code)
{
    if (code == 0 || code == 1) {
        return K_CLEAR;
    }
    if (code == 2) {
        return K_PARTLY;
    }
    if (code == 3) {
        return K_CLOUDY;
    }
    if (code == 45 || code == 48) {
        return K_FOG;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return K_SNOW;
    }
    if (code >= 95) {
        return K_STORM;
    }
    return K_RAIN;          // drizzle, rain, showers
}

/* WMO weather interpretation codes, as used by open-meteo */
const char *code_text(int code)
{
    switch (code) {
    case 0:
        return T("Trời quang", "Clear sky");
    case 1:
        return T("Ít mây", "Mainly clear");
    case 2:
        return T("Mây rải rác", "Partly cloudy");
    case 3:
        return T("Nhiều mây", "Overcast");
    case 45:
    case 48:
        return T("Sương mù", "Fog");
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
        return T("Mưa phùn", "Drizzle");
    case 61:
        return T("Mưa nhỏ", "Light rain");
    case 63:
        return T("Mưa vừa", "Rain");
    case 65:
    case 66:
    case 67:
        return T("Mưa to", "Heavy rain");
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return T("Tuyết", "Snow");
    case 80:
    case 81:
        return T("Mưa rào", "Showers");
    case 82:
        return T("Mưa rào to", "Heavy showers");
    case 95:
    case 96:
    case 99:
        return T("Dông", "Thunderstorm");
    default:
        return T("Không rõ", "Unknown");
    }
}

/* ---- icons made of LVGL objects ---- */

lv_obj_t *shape(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius = LV_RADIUS_CIRCLE)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return o;
}

/* Cloud of width w with its top-left corner at (x, y) */
void cloud(lv_obj_t *p, int x, int y, int w, uint32_t color)
{
    shape(p, x, y + w * 30 / 100, w, w * 34 / 100, color);                  // base
    shape(p, x + w * 12 / 100, y + w * 12 / 100, w * 40 / 100, w * 40 / 100, color);
    shape(p, x + w * 38 / 100, y, w * 50 / 100, w * 50 / 100, color);
}

/* s x s icon; the parent lays it out */
lv_obj_t *make_icon(lv_obj_t *parent, Kind kind, bool is_day, int s)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, s, s);
    lv_obj_remove_flag(p, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    const uint32_t disc = is_day ? COLOR_SUN : COLOR_MOON;
    switch (kind) {
    case K_CLEAR:
        shape(p, s * 18 / 100, s * 18 / 100, s * 64 / 100, s * 64 / 100, disc);
        break;
    case K_PARTLY:
        shape(p, s * 42 / 100, s * 8 / 100, s * 50 / 100, s * 50 / 100, disc);
        cloud(p, s * 4 / 100, s * 34 / 100, s * 78 / 100, COLOR_CLOUD);
        break;
    case K_CLOUDY:
        cloud(p, s * 30 / 100, s * 12 / 100, s * 66 / 100, COLOR_CLOUD_DARK);
        cloud(p, s * 4 / 100, s * 30 / 100, s * 80 / 100, COLOR_CLOUD);
        break;
    case K_FOG:
        for (int i = 0; i < 4; i++) {
            shape(p, s * (i % 2 ? 22 : 10) / 100, s * (22 + i * 17) / 100, s * 68 / 100, s * 8 / 100, COLOR_CLOUD_DARK);
        }
        break;
    case K_RAIN:
    case K_STORM:
    case K_SNOW:
        cloud(p, s * 8 / 100, s * 6 / 100, s * 84 / 100, kind == K_STORM ? COLOR_CLOUD_DARK : COLOR_CLOUD);
        for (int i = 0; i < 3; i++) {
            const int x = s * (26 + i * 22) / 100;
            if (kind == K_SNOW) {
                shape(p, x, s * 72 / 100, s * 10 / 100, s * 10 / 100, 0xFFFFFF);
            } else if (kind == K_STORM && i == 1) {
                shape(p, x, s * 64 / 100, s * 8 / 100, s * 30 / 100, COLOR_SUN, 2);     // lightning
            } else {
                shape(p, x, s * 68 / 100, s * 6 / 100, s * 20 / 100, COLOR_RAIN);
            }
        }
        break;
    }
    return p;
}

/* ---- data ---- */

int json_int(const cJSON *obj, const char *key, int def = 0)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? static_cast<int>(lround(v->valuedouble)) : def;
}

int json_at(const cJSON *arr, int i, int def = 0)
{
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsNumber(v) ? static_cast<int>(lround(v->valuedouble)) : def;
}

bool parse(const char *body, Weather &wx)
{
    cJSON *root = cJSON_Parse(body);
    const cJSON *cur = cJSON_GetObjectItem(root, "current");
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    const cJSON *times = cJSON_GetObjectItem(daily, "time");
    const bool ok = cJSON_IsObject(cur) && cJSON_IsArray(times);
    if (ok) {
        wx.temp = json_int(cur, "temperature_2m");
        wx.feels = json_int(cur, "apparent_temperature");
        wx.humidity = json_int(cur, "relative_humidity_2m");
        wx.wind = json_int(cur, "wind_speed_10m");
        wx.code = json_int(cur, "weather_code");
        wx.is_day = json_int(cur, "is_day", 1) != 0;
        const cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
        const cJSON *tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
        const cJSON *tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
        const cJSON *rain = cJSON_GetObjectItem(daily, "precipitation_probability_max");
        wx.days = cJSON_GetArraySize(times) < DAYS ? cJSON_GetArraySize(times) : DAYS;
        for (int i = 0; i < wx.days; i++) {
            const cJSON *t = cJSON_GetArrayItem(times, i);
            strlcpy(wx.day[i].date, cJSON_IsString(t) ? t->valuestring : "", sizeof(wx.day[i].date));
            wx.day[i].code = json_at(codes, i);
            wx.day[i].tmax = json_at(tmax, i);
            wx.day[i].tmin = json_at(tmin, i);
            wx.day[i].rain = json_at(rain, i);
        }
    }
    cJSON_Delete(root);
    return ok;
}

void fetch_task(void *)
{
    const City &c = CITIES[s_fetch_city];
    char url[384];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=auto&forecast_days=%d", c.lat, c.lon, DAYS);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;      /* the request line alone is over 300 bytes: the default 512 is too small */
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    bool ok = false;
    char *body = static_cast<char *>(malloc(BODY_MAX));
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    const esp_err_t err = body && client ? esp_http_client_open(client, 0) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        size_t len = 0;
        int n;
        while (len < BODY_MAX - 1 && (n = esp_http_client_read(client, body + len, BODY_MAX - 1 - len)) > 0) {
            len += n;
        }
        body[len] = '\0';
        const int status = esp_http_client_get_status_code(client);
        s_new = {};
        s_new.city = s_fetch_city;
        ok = status == 200 && parse(body, s_new);
        if (!ok) {
            ESP_LOGE(TAG, "bad answer: HTTP %d, %u bytes", status, static_cast<unsigned>(len));
        }
    } else {
        ESP_LOGE(TAG, "request failed: %s", esp_err_to_name(err));
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(body);
    s_new.fetched = time(nullptr);
    s_fetch = ok ? F_DONE : F_FAILED;
    vTaskDelete(nullptr);
}

/* ---- UI ---- */

void show_message(const char *text, bool with_retry);

void start_fetch()
{
    if (s_fetch == F_BUSY) {
        return;
    }
    phone_net_status_t st = {};
    phone_net_get_status(&st);
    if (!st.wifi_connected) {
        if (!s_have_wx) {
            show_message(T("Chưa kết nối Wi-Fi", "Wi-Fi is not connected"), true);
        }
        lv_label_set_text(s_updated, T("Không có mạng", "Offline"));
        return;
    }
    s_fetch_city = s_city;
    s_fetch = F_BUSY;
    lv_label_set_text(s_updated, T("Đang cập nhật...", "Updating..."));
    if (!s_have_wx) {
        show_message(T("Đang tải...", "Loading..."), false);
    }
    if (xTaskCreate(fetch_task, "phone_weather", 8192, nullptr, 3, nullptr) != pdPASS) {
        s_fetch = F_FAILED;
    }
}

void on_retry(lv_event_t *)
{
    start_fetch();
}

void show_message(const char *text, bool with_retry)
{
    lv_obj_clean(s_content);
    lv_obj_t *l = lv_label_create(s_content);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_pad_top(l, 120, 0);
    lv_label_set_text(l, text);
    if (with_retry) {
        lv_obj_t *b = lv_button_create(s_content);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_pad_hor(b, 28, 0);
        lv_obj_set_style_pad_ver(b, 12, 0);
        lv_obj_add_event_cb(b, on_retry, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text(t, T("Thử lại", "Retry"));
    }
}

/* Day of the week of a YYYY-MM-DD date, 0 = Sunday */
int weekday(const char *date)
{
    int y = 0, m = 0, d = 0;
    if (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) {
        return 0;
    }
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

lv_obj_t *stat(lv_obj_t *parent, const char *title, const char *value)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 2, 0);
    lv_obj_t *t = lv_label_create(box);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(t, phone_font_by_size(16), 0);
    lv_label_set_text(t, title);
    lv_obj_t *v = lv_label_create(box);
    lv_label_set_text(v, value);
    return box;
}

void show_weather()
{
    static const char *const days_vi[] = {"CN", "T2", "T3", "T4", "T5", "T6", "T7"};
    static const char *const days_en[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const Weather &wx = s_wx;
    lv_obj_clean(s_content);

    /* now: icon, temperature, description */
    lv_obj_t *now = lv_obj_create(s_content);
    lv_obj_remove_style_all(now);
    lv_obj_set_size(now, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(now, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(now, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(now, 24, 0);
    make_icon(now, code_kind(wx.code), wx.is_day, 120);
    lv_obj_t *col = lv_obj_create(now);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *temp = lv_label_create(col);
    lv_obj_set_style_text_font(temp, &lv_font_montserrat_44, 0);
    lv_label_set_text_fmt(temp, "%d°C", wx.temp);
    lv_obj_t *desc = lv_label_create(col);
    lv_obj_set_style_text_font(desc, phone_font_by_size(24), 0);
    lv_label_set_text(desc, code_text(wx.code));

    /* feels like, humidity, wind */
    lv_obj_t *stats = lv_obj_create(s_content);
    lv_obj_remove_style_all(stats);
    lv_obj_set_size(stats, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(stats, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(stats, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(stats, 16, 0);
    lv_obj_set_style_pad_ver(stats, 12, 0);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    char buf[24];
    snprintf(buf, sizeof(buf), "%d°C", wx.feels);
    stat(stats, T("Cảm giác", "Feels like"), buf);
    snprintf(buf, sizeof(buf), "%d%%", wx.humidity);
    stat(stats, T("Độ ẩm", "Humidity"), buf);
    snprintf(buf, sizeof(buf), "%d km/h", wx.wind);
    stat(stats, T("Gió", "Wind"), buf);

    /* forecast: day, icon, chance of rain, min / max */
    lv_obj_t *list = lv_obj_create(s_content);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(list, 16, 0);
    lv_obj_set_style_pad_hor(list, 16, 0);
    lv_obj_set_style_pad_ver(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < wx.days; i++) {
        const Day &d = wx.day[i];
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 48);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *name = lv_label_create(row);
        const int wd = weekday(d.date);
        lv_label_set_text(name, i == 0 ? T("Hôm nay", "Today")
                          : (phone_lang_get() == PHONE_LANG_VI ? days_vi[wd] : days_en[wd]));
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_align(make_icon(row, code_kind(d.code), true, 40), LV_ALIGN_LEFT_MID, 118, 0);
        lv_obj_t *rain = lv_label_create(row);
        lv_obj_set_style_text_color(rain, lv_color_hex(d.rain >= 50 ? COLOR_RAIN : COLOR_SUB), 0);
        lv_obj_set_style_text_font(rain, phone_font_by_size(18), 0);
        lv_label_set_text_fmt(rain, "%d%%", d.rain);
        lv_obj_align(rain, LV_ALIGN_LEFT_MID, 190, 0);
        lv_obj_t *range = lv_label_create(row);
        lv_label_set_text_fmt(range, "%d° / %d°", d.tmin, d.tmax);
        lv_obj_align(range, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_t *credit = lv_label_create(s_content);
    lv_obj_set_style_text_color(credit, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(credit, phone_font_by_size(14), 0);
    lv_label_set_text(credit, T("Dữ liệu: open-meteo.com", "Data: open-meteo.com"));

    struct tm tm;
    localtime_r(&wx.fetched, &tm);
    lv_label_set_text_fmt(s_updated, "%s %02d:%02d", T("Cập nhật", "Updated"), tm.tm_hour, tm.tm_min);
}

void set_city_label()
{
    lv_label_set_text_fmt(lv_obj_get_child(s_city_btn, 0), "%s  " LV_SYMBOL_DOWN, CITIES[s_city].name);
}

void on_timer(lv_timer_t *)
{
    if (!s_root) {
        return;
    }
    const int f = s_fetch;
    if (f == F_DONE || f == F_FAILED) {
        s_fetch = F_IDLE;
        if (f == F_DONE && s_new.city == s_city) {
            s_wx = s_new;
            s_have_wx = true;
            show_weather();
        } else if (f == F_DONE) {
            start_fetch();      /* the city changed while the request was running */
        } else if (s_have_wx) {
            lv_label_set_text(s_updated, T("Không cập nhật được", "Update failed"));
        } else {
            show_message(T("Không tải được dữ liệu", "Could not load the weather"), true);
            lv_label_set_text(s_updated, "");
        }
    }
}

void on_city_pick(lv_event_t *e)
{
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    lv_obj_add_flag(s_cities, LV_OBJ_FLAG_HIDDEN);
    if (idx == s_city) {
        return;
    }
    s_city = idx;
    phone_pref_set("wx_city", idx);
    set_city_label();
    s_have_wx = false;
    start_fetch();
}

void on_city_open(lv_event_t *)
{
    lv_obj_remove_flag(s_cities, LV_OBJ_FLAG_HIDDEN);
}

void on_refresh(lv_event_t *)
{
    start_fetch();
}

void build_ui()
{
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(root, lv_color_white(), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(20), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    /* header: city (opens the picker), last update, refresh */
    s_city_btn = lv_button_create(root);
    lv_obj_remove_style_all(s_city_btn);
    lv_obj_set_size(s_city_btn, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(s_city_btn, 14, 0);
    lv_obj_set_style_radius(s_city_btn, 22, 0);
    lv_obj_set_style_bg_opa(s_city_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_city_btn, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_color(s_city_btn, lv_color_hex(0x3A444E), LV_STATE_PRESSED);
    lv_obj_align(s_city_btn, LV_ALIGN_TOP_LEFT, 16, 12);
    lv_obj_add_event_cb(s_city_btn, on_city_open, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(s_city_btn);
    lv_obj_set_style_text_font(cl, phone_font_by_size(24), 0);
    lv_obj_center(cl);
    set_city_label();

    lv_obj_t *refresh = lv_button_create(root);
    lv_obj_remove_style_all(refresh);
    lv_obj_set_size(refresh, 44, 44);
    lv_obj_set_style_radius(refresh, 22, 0);
    lv_obj_set_style_bg_opa(refresh, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(0x3A444E), LV_STATE_PRESSED);
    lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_add_event_cb(refresh, on_refresh, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *rl = lv_label_create(refresh);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_center(rl);

    s_updated = lv_label_create(root);
    lv_obj_set_style_text_color(s_updated, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_updated, phone_font_by_size(16), 0);
    lv_obj_align(s_updated, LV_ALIGN_TOP_RIGHT, -72, 24);
    lv_label_set_text(s_updated, "");

    s_content = lv_obj_create(root);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_top(s_content, 76, 0);
    lv_obj_set_style_pad_hor(s_content, 16, 0);
    lv_obj_set_style_pad_bottom(s_content, 32, 0);
    lv_obj_set_style_pad_row(s_content, 18, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    phone_ui_scrollbar(s_content);
    lv_obj_move_background(s_content);

    /* city picker: covers the page */
    s_cities = lv_obj_create(root);
    lv_obj_remove_style_all(s_cities);
    lv_obj_set_size(s_cities, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_cities, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cities, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_pad_all(s_cities, 16, 0);
    lv_obj_set_style_pad_bottom(s_cities, 32, 0);
    lv_obj_set_style_pad_row(s_cities, 8, 0);
    lv_obj_set_flex_flow(s_cities, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_cities, LV_DIR_VER);
    phone_ui_scrollbar(s_cities);
    lv_obj_add_flag(s_cities, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *title = lv_label_create(s_cities);
    lv_obj_set_style_text_font(title, phone_font_by_size(24), 0);
    lv_obj_set_style_pad_bottom(title, 6, 0);
    lv_label_set_text(title, T("Chọn thành phố", "Choose a city"));
    for (int i = 0; i < CITY_COUNT; i++) {
        lv_obj_t *b = lv_button_create(s_cities);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_PCT(100), 52);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_pad_hor(b, 16, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i == s_city ? COLOR_ACCENT : COLOR_CARD), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A444E), LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, on_city_pick, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, CITIES[i].name);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    }

    /* The strip where the swipe-up for the navigation bar starts: a touch there must not scroll
     * the list under it. The system reads that gesture straight from the touch panel. */
    lv_obj_t *guard = lv_obj_create(root);
    lv_obj_remove_style_all(guard);
    lv_obj_set_size(guard, LV_PCT(100), 36);
    lv_obj_align(guard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(guard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(guard, LV_OBJ_FLAG_SCROLLABLE);
}

/* Show what is known, and fetch when there is nothing or it is old */
void refresh_if_needed()
{
    if (s_have_wx && s_wx.city == s_city) {
        show_weather();
        if (time(nullptr) - s_wx.fetched < REFRESH_S) {
            return;
        }
    } else {
        s_have_wx = false;
    }
    start_fetch();
}

} // namespace

namespace esp_brookesia::apps {

PhoneWeather *PhoneWeather::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneWeather();
    }
    return _instance;
}

PhoneWeather::PhoneWeather():
    App(phone_tr("Thời tiết", "Weather"), &phone_weather_icon, true, true, true)
{
}

PhoneWeather::~PhoneWeather()
{
}

bool PhoneWeather::run()
{
    s_city = phone_pref_get("wx_city", CITY_DEFAULT);
    if (s_city < 0 || s_city >= CITY_COUNT) {
        s_city = CITY_DEFAULT;
    }
    build_ui();
    lv_timer_create(on_timer, 200, nullptr);
    refresh_if_needed();
    return true;
}

bool PhoneWeather::back()
{
    if (s_cities && !lv_obj_has_flag(s_cities, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_cities, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    return notifyCoreClosed();
}

bool PhoneWeather::resume()
{
    if (s_root) {
        refresh_if_needed();
    }
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneWeather, APP_NAME, []()
{
    return std::shared_ptr<PhoneWeather>(PhoneWeather::requestInstance(), [](PhoneWeather * p) {});
})

} // namespace esp_brookesia::apps
