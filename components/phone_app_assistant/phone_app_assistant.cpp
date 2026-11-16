/*
 * Assistant app: voice chat with the xiaozhi.me service, through the esp_xiaozhi component.
 *
 * This first step covers the connection and the activation of the device:
 *   - a task asks the server for the device configuration (esp_xiaozhi_chat_get_info, HTTPS);
 *   - when the device is not bound to an account yet, the server answers with a 6-digit code:
 *     the code is shown, and the request is repeated until the user has entered it on the site;
 *   - once activated, the app shows that it is ready and which transports the server offers.
 * The conversation itself (microphone, Opus, speaker, chat bubbles) comes next.
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_xiaozhi_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "ASSIST"
#include "esp_lib_utils.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_net.h"
#include "phone_app_assistant.hpp"

#define APP_NAME "Trợ lý AI"

extern "C" const lv_image_dsc_t phone_assistant_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_assistant";

constexpr uint32_t ACTIVATION_POLL_MS = 10000;  // between two checks while waiting for the code to be entered
constexpr uint32_t RETRY_MS = 3000;             // after a failed request
constexpr int MAX_FAILURES = 5;                 // failed requests in a row before giving up (none once a code is shown)
constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_CARD = 0x1E252C;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0x6C7CFF;
constexpr uint32_t COLOR_OK = 0x22C55E;
constexpr uint32_t COLOR_ERROR = 0xF87171;

enum State { ST_IDLE, ST_NO_WIFI, ST_FETCHING, ST_ACTIVATE, ST_READY, ST_ERROR };

lv_obj_t *s_root;
lv_obj_t *s_face;
lv_obj_t *s_status;
lv_obj_t *s_code_card;
lv_obj_t *s_code;
lv_obj_t *s_hint;
lv_obj_t *s_retry;
int s_shown_state = -1;
char s_shown_code[16];

std::atomic<int> s_state{ST_IDLE};
std::atomic<uint32_t> s_generation;         // a newer run of the task makes the older one stop
std::atomic<bool> s_task_running;
std::atomic<bool> s_has_mqtt, s_has_websocket;
char s_code_text[16];                       // written by the task before it sets ST_ACTIVATE

/* Ask the server until the device is activated; ends when ready, on error, or when superseded */
void info_task(void *arg)
{
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    auto current = [generation]() {
        return s_generation == generation;
    };
    int failures = 0;
    bool have_code = false;
    while (current()) {
        phone_net_status_t net = {};
        phone_net_get_status(&net);
        if (!net.wifi_connected) {
            if (!have_code) {
                s_state = ST_NO_WIFI;       /* with a code on the screen, keep it: the link usually comes back */
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!have_code) {
            s_state = ST_FETCHING;
        }
        esp_xiaozhi_chat_info_t info = {};
        const esp_err_t err = esp_xiaozhi_chat_get_info(&info);
        if (!current()) {
            esp_xiaozhi_chat_free_info(&info);
            break;
        }
        if (err != ESP_OK) {
            /* a dropped connection is common on Wi-Fi: try again, and never drop a code already shown */
            ESP_LOGW(TAG, "get_info: %s (%d in a row)", esp_err_to_name(err), failures + 1);
            esp_xiaozhi_chat_free_info(&info);
            if (!have_code && ++failures >= MAX_FAILURES) {
                s_state = ST_ERROR;
                break;
            }
            for (uint32_t waited = 0; waited < RETRY_MS && current(); waited += 100) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        failures = 0;
        if (info.has_activation_code || info.has_activation_challenge) {
            strlcpy(s_code_text, info.activation_code ? info.activation_code : "", sizeof(s_code_text));
            ESP_LOGI(TAG, "not activated yet, code received");
            have_code = true;
            s_state = ST_ACTIVATE;
            esp_xiaozhi_chat_free_info(&info);
            for (uint32_t waited = 0; waited < ACTIVATION_POLL_MS && current(); waited += 100) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        s_has_mqtt = info.has_mqtt_config;
        s_has_websocket = info.has_websocket_config;
        ESP_LOGI(TAG, "activated: mqtt %d, websocket %d", info.has_mqtt_config, info.has_websocket_config);
        esp_xiaozhi_chat_free_info(&info);
        s_state = ST_READY;
        break;
    }
    if (current()) {
        s_task_running = false;
    }
    vTaskDelete(nullptr);
}

void start_info_task()
{
    const uint32_t generation = ++s_generation;
    s_state = ST_FETCHING;
    s_task_running = true;
    if (xTaskCreate(info_task, "assistant_info", 8192, reinterpret_cast<void *>(static_cast<uintptr_t>(generation)), 4,
                    nullptr) != pdPASS) {
        s_task_running = false;
        s_state = ST_ERROR;
    }
}

void on_retry(lv_event_t *)
{
    start_info_task();
}

void set_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

void on_timer(lv_timer_t *)
{
    if (!s_root) {
        return;
    }
    const int state = s_state;
    if (state == ST_ACTIVATE && strcmp(s_shown_code, s_code_text) != 0) {
        /* the code, spaced out so that it is easy to read aloud and to type */
        strlcpy(s_shown_code, s_code_text, sizeof(s_shown_code));
        char spaced[40] = "";
        size_t n = 0;
        for (const char *c = s_shown_code; *c && n + 3 < sizeof(spaced); c++) {
            spaced[n++] = *c;
            spaced[n++] = ' ';
        }
        spaced[n ? n - 1 : 0] = '\0';
        lv_label_set_text(s_code, spaced);
    }
    if (state == s_shown_state) {
        return;
    }
    s_shown_state = state;
    lv_obj_set_flag(s_code_card, LV_OBJ_FLAG_HIDDEN, state != ST_ACTIVATE);
    lv_obj_set_flag(s_retry, LV_OBJ_FLAG_HIDDEN, state != ST_ERROR);
    lv_obj_set_style_bg_color(s_face, lv_color_hex(state == ST_READY ? COLOR_ACCENT : 0x3A444E), 0);
    switch (state) {
    case ST_NO_WIFI:
        set_status(T("Chưa kết nối Wi-Fi", "Wi-Fi is not connected"), COLOR_SUB);
        lv_label_set_text(s_hint, T("Trợ lý cần Internet. Hãy kết nối Wi-Fi trong Cài đặt.",
                                    "The assistant needs the Internet. Connect to Wi-Fi in Settings."));
        break;
    case ST_FETCHING:
        set_status(T("Đang kết nối máy chủ...", "Contacting the server..."), COLOR_SUB);
        lv_label_set_text(s_hint, "");
        break;
    case ST_ACTIVATE:
        set_status(T("Cần kích hoạt thiết bị", "The device must be activated"), COLOR_ACCENT);
        lv_label_set_text(s_hint, T("Vào xiaozhi.me, đăng nhập, mở Bảng điều khiển, chọn thêm thiết bị và nhập mã này. "
                                    "Màn hình sẽ tự chuyển khi kích hoạt xong.",
                                    "Open xiaozhi.me, sign in, open the console, add a device and enter this code. "
                                    "This page moves on by itself once the device is activated."));
        break;
    case ST_READY:
        set_status(T("Đã sẵn sàng", "Ready"), COLOR_OK);
        lv_label_set_text_fmt(s_hint, "%s: %s%s%s\n\n%s", T("Kênh của máy chủ", "Server transports"),
                              s_has_mqtt ? "MQTT + UDP" : "", s_has_mqtt && s_has_websocket ? ", " : "",
                              s_has_websocket ? "WebSocket" : "",
                              T("Thiết bị đã được kích hoạt. Phần hội thoại bằng giọng nói sẽ có ở bước tiếp theo.",
                                "The device is activated. The voice conversation comes in the next step."));
        break;
    case ST_ERROR:
        set_status(T("Không kết nối được máy chủ", "Could not reach the server"), COLOR_ERROR);
        lv_label_set_text(s_hint, T("Kiểm tra Wi-Fi có Internet không rồi thử lại.",
                                    "Check that the Wi-Fi has Internet access, then try again."));
        break;
    default:
        set_status("", COLOR_SUB);
        break;
    }
}

void build_ui()
{
    s_shown_state = -1;
    s_shown_code[0] = '\0';

    lv_obj_t *root = lv_obj_create(lv_screen_active());
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(root, lv_color_white(), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(20), 0);
    lv_obj_set_style_pad_all(root, 16, 0);
    lv_obj_set_style_pad_bottom(root, 52, 0);
    lv_obj_set_style_pad_row(root, 16, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_style_text_font(title, phone_font_by_size(28), 0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_text(title, T("Trợ lý AI", "AI Assistant"));

    /* placeholder of the animated face: a disc with two eyes */
    s_face = lv_obj_create(root);
    lv_obj_remove_style_all(s_face);
    lv_obj_set_size(s_face, 150, 150);
    lv_obj_set_style_radius(s_face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_face, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_face, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_margin_top(s_face, 12, 0);
    lv_obj_remove_flag(s_face, LV_OBJ_FLAG_SCROLLABLE);
    for (int dx : {-30, 30}) {
        lv_obj_t *eye = lv_obj_create(s_face);
        lv_obj_remove_style_all(eye);
        lv_obj_set_size(eye, 18, 34);
        lv_obj_set_style_radius(eye, 9, 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_align(eye, LV_ALIGN_CENTER, dx, -6);
    }

    s_status = lv_label_create(root);
    lv_obj_set_style_text_font(s_status, phone_font_by_size(24), 0);
    lv_label_set_text(s_status, "");

    s_code_card = lv_obj_create(root);
    lv_obj_remove_style_all(s_code_card);
    lv_obj_set_size(s_code_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_code_card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_code_card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(s_code_card, 18, 0);
    lv_obj_set_style_pad_ver(s_code_card, 18, 0);
    lv_obj_set_flex_flow(s_code_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_code_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_code_card, 6, 0);
    lv_obj_remove_flag(s_code_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_code_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *code_title = lv_label_create(s_code_card);
    lv_obj_set_style_text_color(code_title, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(code_title, phone_font_by_size(18), 0);
    lv_label_set_text(code_title, T("Mã kích hoạt", "Activation code"));
    s_code = lv_label_create(s_code_card);
    lv_obj_set_style_text_font(s_code, &lv_font_montserrat_44, 0);
    lv_label_set_text(s_code, "");

    s_hint = lv_label_create(root);
    lv_obj_set_width(s_hint, LV_PCT(100));
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_hint, phone_font_by_size(18), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_hint, 6, 0);
    lv_label_set_text(s_hint, "");

    s_retry = lv_button_create(root);
    lv_obj_remove_style_all(s_retry);
    lv_obj_set_size(s_retry, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_pad_hor(s_retry, 32, 0);
    lv_obj_set_style_radius(s_retry, 16, 0);
    lv_obj_set_style_bg_opa(s_retry, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_retry, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_retry, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_retry, on_retry, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_retry, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *rl = lv_label_create(s_retry);
    lv_label_set_text(rl, T("Thử lại", "Retry"));
    lv_obj_center(rl);
}

} // namespace

namespace esp_brookesia::apps {

PhoneAssistant *PhoneAssistant::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneAssistant();
    }
    return _instance;
}

PhoneAssistant::PhoneAssistant():
    App(phone_tr("Trợ lý AI", "AI Assistant"), &phone_assistant_icon, true, true, true)
{
}

PhoneAssistant::~PhoneAssistant()
{
}

bool PhoneAssistant::run()
{
    build_ui();
    lv_timer_create(on_timer, 200, nullptr);
    if (s_state != ST_READY) {
        start_info_task();
    }
    return true;
}

bool PhoneAssistant::back()
{
    return notifyCoreClosed();
}

/* The task may be inside an HTTPS request: it is told to stop and ends by itself */
bool PhoneAssistant::close()
{
    ++s_generation;
    if (s_state != ST_READY) {
        s_state = ST_IDLE;
    }
    return true;
}

/* Back in front: check again unless the device is known to be activated */
bool PhoneAssistant::resume()
{
    if (s_root && s_state != ST_READY && !s_task_running) {
        start_info_task();
    }
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneAssistant, APP_NAME, []()
{
    return std::shared_ptr<PhoneAssistant>(PhoneAssistant::requestInstance(), [](PhoneAssistant * p) {});
})

} // namespace esp_brookesia::apps
