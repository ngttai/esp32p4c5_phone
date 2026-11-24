/*
 * Second screen app: the board works as a 480 x 800 USB display with touch for a PC.
 *
 * Protocol: "udisp" of the xfz1986 USB graphic driver (Windows IDD driver, Linux DRM driver).
 * The board is a high-speed USB device 303A:2986 with
 *   - a vendor interface: the PC sends each screen update as a 16-byte header followed by a JPEG
 *     picture, on a bulk OUT endpoint. The interface string tells the driver the resolution and
 *     the JPEG quality: "esp32p4udisp0_R480x800_Ejpg5";
 *   - a HID touch screen, fed with the touches on the picture.
 * JPEG pictures are decoded by the hardware decoder into one of two RGB565 buffers, shown by an
 * LVGL image on the system layer, above the status and navigation bars.
 *
 * Landscape mode tells the PC 800 x 480 instead: the decoded picture is turned 90 degrees by the
 * PPA and the touches are turned back, so the board is used on its side.
 *
 * Connect the PC to the USB HS Type-C socket. The port is shared with the Type-A host socket.
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "class/hid/hid_device.h"
#include "class/vendor/vendor_device.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "SCREEN2"
#include "esp_lib_utils.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"
#include "phone_app_second_screen.hpp"

#define APP_NAME "Màn hình phụ"

extern "C" const lv_image_dsc_t phone_second_screen_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_screen2";

constexpr int SCREEN_W = 480;            // panel
constexpr int SCREEN_H = 800;
/* How the landscape picture is turned onto the portrait panel (counter-clockwise), as in the Video app */
constexpr ppa_srm_rotation_angle_t LANDSCAPE_ROTATION = PPA_SRM_ROTATION_ANGLE_90;
constexpr int JPEG_QUALITY = 5;             // 1..10, asked from the PC driver
constexpr size_t FRAME_MAX = 300 * 1024;    // larger pictures are skipped
constexpr int TOUCH_POINTS = 5;             // fingers in the HID report (one is used)

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_CARD = 0x1E252C;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0x21B5B5;

/* ---- USB descriptors ---- */

enum { ITF_VENDOR, ITF_HID, ITF_TOTAL };
enum { EP_VENDOR = 1, EP_HID = 2 };
enum { STR_VENDOR = 4, STR_HID };
enum { REPORT_TOUCH = 1, REPORT_MAX_COUNT };

#define FINGER_USAGE(width, height)                                            \
    HID_USAGE(0x42), HID_COLLECTION(HID_COLLECTION_LOGICAL), HID_USAGE(0x42),  \
        HID_LOGICAL_MIN(0x00), HID_LOGICAL_MAX(0x01), HID_REPORT_SIZE(1),      \
        HID_REPORT_COUNT(1), HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), \
        HID_REPORT_COUNT(7),                                                   \
        HID_INPUT(HID_CONSTANT | HID_ARRAY | HID_ABSOLUTE),                    \
        HID_REPORT_SIZE(8), HID_USAGE(0x51), HID_REPORT_COUNT(1),              \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                     \
        HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),                                \
        HID_LOGICAL_MAX_N(width, 2), HID_REPORT_SIZE(16),                      \
        HID_UNIT_EXPONENT(0x0e), HID_UNIT(0x13), HID_USAGE(0x30),              \
        HID_PHYSICAL_MIN(0), HID_PHYSICAL_MAX_N(width, 2),                     \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                     \
        HID_LOGICAL_MAX_N(height, 2), HID_PHYSICAL_MAX_N(height, 2),           \
        HID_USAGE(0x31), HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),    \
        HID_USAGE_PAGE(HID_USAGE_PAGE_DIGITIZER), HID_USAGE(0x48),             \
        HID_USAGE(0x49), HID_REPORT_COUNT(2),                                  \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), HID_COLLECTION_END,

/* Touch screen with five contacts and the "contact count maximum" feature */
#define TOUCH_SCREEN_DESC(width, height)                                                              \
    HID_USAGE_PAGE(HID_USAGE_PAGE_DIGITIZER), HID_USAGE(0x04),                                        \
    HID_COLLECTION(HID_COLLECTION_APPLICATION), HID_REPORT_ID(REPORT_TOUCH)                           \
    FINGER_USAGE(width, height) FINGER_USAGE(width, height) FINGER_USAGE(width, height)               \
    FINGER_USAGE(width, height) FINGER_USAGE(width, height) HID_USAGE(0x54),                          \
    HID_LOGICAL_MAX(127), HID_REPORT_COUNT(1), HID_REPORT_SIZE(8),                                    \
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                                                \
    HID_REPORT_ID(REPORT_MAX_COUNT) HID_USAGE(0x55), HID_REPORT_COUNT(1),                             \
    HID_LOGICAL_MAX(0x10),                                                                            \
    HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),                                              \
    HID_COLLECTION_END

/* The touch area is the screen as the PC sees it: one descriptor per orientation, same length */
const uint8_t HID_REPORT_DESC[] = {TOUCH_SCREEN_DESC(SCREEN_W, SCREEN_H)};
const uint8_t HID_REPORT_DESC_LANDSCAPE[] = {TOUCH_SCREEN_DESC(SCREEN_H, SCREEN_W)};
static_assert(sizeof(HID_REPORT_DESC) == sizeof(HID_REPORT_DESC_LANDSCAPE), "the configuration descriptor holds one length");

constexpr int CONFIG_LEN = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_HID_DESC_LEN;

const uint8_t CONFIG_DESC_HS[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR, STR_VENDOR, EP_VENDOR, 0x80 | EP_VENDOR, 512),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof(HID_REPORT_DESC), 0x80 | EP_HID, 64, 4),
};

const uint8_t CONFIG_DESC_FS[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR, STR_VENDOR, EP_VENDOR, 0x80 | EP_VENDOR, 64),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof(HID_REPORT_DESC), 0x80 | EP_HID, 64, 10),
};

const tusb_desc_device_t DEVICE_DESC = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceProtocol = TUSB_CLASS_UNSPECIFIED,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x2986,
    .bcdDevice = 0x0101,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const tusb_desc_device_qualifier_t QUALIFIER_DESC = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceProtocol = TUSB_CLASS_UNSPECIFIED,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};

/* esp_tinyusb sends at most 31 characters of a string: keep the vendor string short */
char s_vendor_str[32];
const char *s_strings[] = {
    (const char[]){0x09, 0x04},
    "Espressif",
    "ESP_Extern_Screen",
    "012-2021",
    s_vendor_str,
    "touch",
};

/* ---- picture path ---- */

/* Header in front of every picture */
struct __attribute__((packed)) FrameHeader {
    uint16_t crc16;
    uint8_t type;
    uint8_t cmd;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t frame_id : 10;
    uint32_t payload_total : 22;
};
constexpr uint8_t TYPE_JPG = 3;

lv_obj_t *s_root;
lv_obj_t *s_start_btn;
lv_obj_t *s_mode_btn[2];
lv_obj_t *s_info;
lv_obj_t *s_view;           // full screen, on the system layer, while running
lv_obj_t *s_img;
lv_obj_t *s_wait;
lv_image_dsc_t s_dsc[2];

bool s_running;
bool s_usb_installed;
TaskHandle_t s_decode_task;
std::atomic<bool> s_decode_run;
std::atomic<bool> s_decode_done;
jpeg_decoder_handle_t s_dec;
ppa_client_handle_t s_ppa;
bool s_landscape;           // the PC sees 800 x 480
int s_host_w, s_host_h;     // screen size told to the PC
uint8_t *s_turn;            // decoded landscape picture, before it is turned
size_t s_turn_size;
uint8_t *s_in[2];           // JPEG data: one being received, one being decoded
size_t s_in_size;
uint8_t *s_out[2];          // RGB565 pictures: one on the screen, one being decoded
size_t s_out_size;
std::atomic<int> s_ready{-1};       // input buffer with a complete picture, -1 when none
std::atomic<uint32_t> s_ready_len;
std::atomic<uint32_t> s_frames;     // pictures shown
std::atomic<uint32_t> s_dropped;

/* receive state, only touched by the TinyUSB task */
int s_rx_buf;               // input buffer being filled
uint32_t s_rx_total, s_rx_got;
bool s_rx_active, s_rx_skip;

void decode_task(void *)
{
    int back = 0;
    int64_t stat_us = esp_timer_get_time();
    uint32_t stat_frames = 0, stat_dropped = 0;
    const jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    while (s_decode_run) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) == 0) {
            continue;
        }
        const int in = s_ready;
        if (in < 0) {
            continue;
        }
        uint32_t len = 0;
        jpeg_decode_picture_info_t info = {};
        bool ok = jpeg_decoder_get_info(s_in[in], s_ready_len, &info) == ESP_OK &&
                  static_cast<int>(info.width) == s_host_w && static_cast<int>(info.height) == s_host_h;
        if (ok && s_landscape) {
            /* decode as sent, then turn it onto the portrait panel */
            ppa_srm_oper_config_t op = {};
            op.in.buffer = s_turn;
            op.in.pic_w = s_host_w;
            op.in.pic_h = s_host_h;
            op.in.block_w = s_host_w;
            op.in.block_h = s_host_h;
            op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            op.out.buffer = s_out[back];
            op.out.buffer_size = s_out_size;
            op.out.pic_w = SCREEN_W;
            op.out.pic_h = SCREEN_H;
            op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            op.rotation_angle = LANDSCAPE_ROTATION;
            op.scale_x = 1.0f;
            op.scale_y = 1.0f;
            op.mode = PPA_TRANS_MODE_BLOCKING;
            ok = jpeg_decoder_process(s_dec, &cfg, s_in[in], s_ready_len, s_turn, s_turn_size, &len) == ESP_OK &&
                 ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK;
        } else if (ok) {
            ok = jpeg_decoder_process(s_dec, &cfg, s_in[in], s_ready_len, s_out[back], s_out_size, &len) == ESP_OK;
        }
        if (ok) {
            if (bsp_display_lock(50)) {
                if (s_img && s_decode_run) {
                    lv_image_set_src(s_img, &s_dsc[back]);
                    back ^= 1;
                    s_frames++;
                }
                bsp_display_unlock();
            }
            /* shown and dropped pictures, every 3 s, for benchmarks */
            const int64_t now = esp_timer_get_time();
            if (now - stat_us >= 3000000) {
                const uint32_t frames = s_frames, dropped = s_dropped;
                ESP_LOGI(TAG, "shown %.1f fps, dropped %.1f fps, last picture %u bytes",
                         (frames - stat_frames) * 1e6 / (now - stat_us), (dropped - stat_dropped) * 1e6 / (now - stat_us),
                         static_cast<unsigned>(s_ready_len.load()));
                stat_us = now;
                stat_frames = frames;
                stat_dropped = dropped;
            }
        } else {
            s_dropped++;
        }
        s_ready = -1;       /* the receiver may use this buffer again */
    }
    s_decode_done = true;
    vTaskDelete(nullptr);
}

/* A complete picture is in the receive buffer: hand it to the decoder if it is free */
void rx_frame_done()
{
    if (s_ready < 0 && s_decode_task) {
        s_ready_len = s_rx_got;
        s_ready = s_rx_buf;
        s_rx_buf ^= 1;
        xTaskNotifyGive(s_decode_task);
    } else {
        s_dropped++;        /* still decoding the previous one */
    }
}

/* ---- touch ---- */

struct __attribute__((packed)) TouchPoint {
    uint8_t press_down;
    uint8_t index;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
};

struct __attribute__((packed)) TouchReport {
    TouchPoint data[TOUCH_POINTS];
    uint8_t cnt;
};

bool s_touch_down;
bool s_touch_sent_down;
bool s_touch_dirty;
lv_point_t s_touch_pt;

void send_touch()
{
    if (!s_usb_installed || !tud_mounted() || !tud_hid_n_ready(0)) {
        return;
    }
    if (!s_touch_down && !s_touch_sent_down) {
        s_touch_dirty = false;
        return;
    }
    TouchReport report = {};
    if (s_touch_down) {
        report.data[0].press_down = 1;
        report.data[0].index = 0;
        if (s_landscape) {
            /* back through the 90 degree turn: the left edge of the panel is the top of the PC screen */
            report.data[0].x = static_cast<uint16_t>(SCREEN_H - 1 - s_touch_pt.y);
            report.data[0].y = static_cast<uint16_t>(s_touch_pt.x);
        } else {
            report.data[0].x = static_cast<uint16_t>(s_touch_pt.x);
            report.data[0].y = static_cast<uint16_t>(s_touch_pt.y);
        }
        report.data[0].width = 8;
        report.data[0].height = 8;
        report.cnt = 1;
    }
    if (tud_hid_n_report(0, REPORT_TOUCH, &report, sizeof(report))) {
        s_touch_sent_down = s_touch_down;
        s_touch_dirty = false;
    }
}

void on_view_touch(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev && (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        p.x = LV_CLAMP(0, p.x, SCREEN_W - 1);
        p.y = LV_CLAMP(0, p.y, SCREEN_H - 1);
        if (!s_touch_down || p.x != s_touch_pt.x || p.y != s_touch_pt.y) {
            s_touch_pt = p;
            s_touch_down = true;
            s_touch_dirty = true;
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_touch_down = false;
        s_touch_dirty = true;
    }
}

/* ---- start / stop ---- */

void free_buffers()
{
    for (int i = 0; i < 2; i++) {
        free(s_in[i]);
        free(s_out[i]);
        s_in[i] = s_out[i] = nullptr;
    }
    free(s_turn);
    s_turn = nullptr;
    if (s_ppa) {
        ppa_unregister_client(s_ppa);
        s_ppa = nullptr;
    }
    if (s_dec) {
        jpeg_del_decoder_engine(s_dec);
        s_dec = nullptr;
    }
}

void stop_screen()
{
    if (s_usb_installed) {
        tinyusb_driver_uninstall();
        s_usb_installed = false;
    }
    if (s_decode_task) {
        s_decode_run = false;
        for (int i = 0; i < 100 && !s_decode_done; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_decode_task = nullptr;
    }
    if (s_view) {
        lv_obj_delete(s_view);
        s_view = nullptr;
        s_img = nullptr;
    }
    free_buffers();
    s_running = false;
    if (s_root) {
        lv_label_set_text(lv_obj_get_child(s_start_btn, 0), T("Bắt đầu", "Start"));
    }
}

void on_close(lv_event_t *)
{
    stop_screen();
}

bool start_screen()
{
    const jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    const jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    const jpeg_decode_engine_cfg_t eng_cfg = {.intr_priority = 0, .timeout_ms = 200};
    for (int i = 0; i < 2; i++) {
        s_in[i] = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(FRAME_MAX, &in_cfg, &s_in_size));
        s_out[i] = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(static_cast<size_t>(SCREEN_W) * SCREEN_H * 2,
                                          &out_cfg, &s_out_size));
        if (!s_in[i] || !s_out[i]) {
            return false;
        }
        memset(s_out[i], 0, s_out_size);
        s_dsc[i] = {};
        s_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_dsc[i].header.w = SCREEN_W;
        s_dsc[i].header.h = SCREEN_H;
        s_dsc[i].header.stride = SCREEN_W * 2;
        s_dsc[i].data_size = SCREEN_W * SCREEN_H * 2;
        s_dsc[i].data = s_out[i];
    }
    if (jpeg_new_decoder_engine(&eng_cfg, &s_dec) != ESP_OK) {
        return false;
    }
    s_host_w = s_landscape ? SCREEN_H : SCREEN_W;
    s_host_h = s_landscape ? SCREEN_W : SCREEN_H;
    if (s_landscape) {
        const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};
        s_turn = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(static_cast<size_t>(s_host_w) * s_host_h * 2, &out_cfg,
                                        &s_turn_size));
        if (!s_turn || ppa_register_client(&ppa_cfg, &s_ppa) != ESP_OK) {
            return false;
        }
    }
    s_ready = -1;
    s_frames = 0;
    s_dropped = 0;
    s_rx_buf = 0;
    s_rx_active = s_rx_skip = false;
    s_touch_down = s_touch_sent_down = s_touch_dirty = false;
    s_decode_run = true;
    s_decode_done = false;
    if (xTaskCreate(decode_task, "screen2_dec", 4096, nullptr, 5, &s_decode_task) != pdPASS) {
        s_decode_task = nullptr;
        return false;
    }

    /* the picture covers everything, bars included; its touches go to the PC */
    s_view = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_view);
    lv_obj_set_size(s_view, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_opa(s_view, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_view, lv_color_black(), 0);
    lv_obj_set_style_text_color(s_view, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_view, phone_font_by_size(20), 0);
    lv_obj_remove_flag(s_view, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_flag(s_view, LV_OBJ_FLAG_CLICKABLE);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(s_view, on_view_touch, code, nullptr);
    }
    s_img = lv_image_create(s_view);
    lv_image_set_src(s_img, &s_dsc[0]);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    s_wait = lv_label_create(s_view);
    lv_obj_set_style_text_align(s_wait, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_wait, lv_color_hex(COLOR_SUB), 0);
    lv_label_set_text(s_wait, T("Đang chờ máy tính...\nCắm cáp vào cổng USB HS (Type-C).",
                                "Waiting for the PC...\nPlug the cable into the USB HS (Type-C) socket."));
    lv_obj_center(s_wait);
    /* the only control on the picture: close */
    lv_obj_t *close = lv_button_create(s_view);
    lv_obj_remove_style_all(close);
    lv_obj_set_size(close, 40, 40);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(close, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(close, lv_color_black(), 0);
    lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *x = lv_label_create(close);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);

    snprintf(s_vendor_str, sizeof(s_vendor_str), "esp32p4udisp0_R%dx%d_Ejpg%d", s_host_w, s_host_h, JPEG_QUALITY);
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &DEVICE_DESC;
    cfg.descriptor.qualifier = &QUALIFIER_DESC;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    cfg.descriptor.full_speed_config = CONFIG_DESC_FS;
    cfg.descriptor.high_speed_config = CONFIG_DESC_HS;
    const esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return false;
    }
    s_usb_installed = true;
    s_running = true;
    ESP_LOGI(TAG, "USB display up: %s", s_vendor_str);
    return true;
}

void on_start(lv_event_t *)
{
    if (s_running) {
        return;
    }
    if (!start_screen()) {
        stop_screen();
        lv_label_set_text(s_info, T("Không khởi động được USB.", "Could not start the USB device."));
    }
}

void on_timer(lv_timer_t *)
{
    if (!s_running) {
        return;
    }
    if (phone_lock_is_locked()) {
        stop_screen();      /* the power key locked the phone */
        return;
    }
    if (s_touch_dirty || s_touch_down) {
        send_touch();
    }
    static uint32_t shown;
    const uint32_t frames = s_frames;
    if (frames != shown) {
        shown = frames;
        if (s_wait && !lv_obj_has_flag(s_wait, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_wait, LV_OBJ_FLAG_HIDDEN);
        }
        lv_display_trigger_activity(nullptr);       /* in use: keep the screen on */
    }
}

void show_mode()
{
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(s_mode_btn[i], lv_color_hex(i == (s_landscape ? 1 : 0) ? COLOR_ACCENT : COLOR_CARD), 0);
    }
}

void on_mode(lv_event_t *e)
{
    if (s_running) {
        return;
    }
    s_landscape = reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0;
    phone_pref_set("ss_land", s_landscape ? 1 : 0);
    show_mode();
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
    lv_obj_set_style_pad_all(root, 16, 0);
    lv_obj_set_style_pad_bottom(root, 52, 0);
    lv_obj_set_style_pad_row(root, 16, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_style_text_font(title, phone_font_by_size(28), 0);
    lv_label_set_text(title, T("Màn hình phụ", "Second screen"));

    lv_obj_t *card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    s_info = lv_label_create(card);
    lv_obj_set_width(s_info, LV_PCT(100));
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_info, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_info, phone_font_by_size(18), 0);
    lv_obj_set_style_text_line_space(s_info, 6, 0);
    lv_label_set_text(s_info, T(
                          "Máy trở thành màn hình USB có cảm ứng cho máy tính, dọc 480 x 800 hoặc ngang 800 x 480.\n\n"
                          "1. Cài driver xfz1986 udisp trên máy tính (Windows hoặc Linux).\n"
                          "2. Cắm cáp vào cổng USB HS (Type-C) của máy.\n"
                          "3. Bấm Bắt đầu.\n\n"
                          "Thoát: bấm nút X ở góc trên phải, hoặc bấm phím nguồn.",
                          "The board becomes a USB display with touch for a PC, 480 x 800 portrait or 800 x 480 landscape.\n\n"
                          "1. Install the xfz1986 udisp driver on the PC (Windows or Linux).\n"
                          "2. Plug the cable into the USB HS (Type-C) socket of the board.\n"
                          "3. Press Start.\n\n"
                          "To leave: tap the X in the top right corner, or press the power key."));

    /* portrait or landscape, as the PC sees the screen */
    lv_obj_t *modes = lv_obj_create(root);
    lv_obj_remove_style_all(modes);
    lv_obj_set_size(modes, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(modes, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(modes, 12, 0);
    static const char *const labels_vi[] = {"Dọc 480 x 800", "Ngang 800 x 480"};
    static const char *const labels_en[] = {"Portrait 480 x 800", "Landscape 800 x 480"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(modes);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, 52);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(b, phone_font_by_size(18), 0);
        lv_obj_add_event_cb(b, on_mode, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, phone_lang_get() == PHONE_LANG_VI ? labels_vi[i] : labels_en[i]);
        lv_obj_center(l);
        s_mode_btn[i] = b;
    }
    show_mode();

    s_start_btn = lv_button_create(root);
    lv_obj_remove_style_all(s_start_btn);
    lv_obj_set_size(s_start_btn, LV_PCT(100), 60);
    lv_obj_set_style_radius(s_start_btn, 16, 0);
    lv_obj_set_style_bg_opa(s_start_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_start_btn, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_start_btn, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_start_btn, on_start, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(s_start_btn);
    lv_label_set_text(l, T("Bắt đầu", "Start"));
    lv_obj_center(l);
}

} // namespace

/* ---- TinyUSB callbacks ---- */

extern "C" {

    uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
    {
        (void)instance;
        return s_landscape ? HID_REPORT_DESC_LANDSCAPE : HID_REPORT_DESC;
    }

    uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                                   uint16_t reqlen)
    {
        (void)itf;
        (void)report_type;
        if (report_id == REPORT_MAX_COUNT && reqlen >= 1) {
            buffer[0] = TOUCH_POINTS;
            return 1;
        }
        return 0;
    }

    void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                               uint16_t bufsize)
    {
        (void)itf;
        (void)report_id;
        (void)report_type;
        (void)buffer;
        (void)bufsize;
    }

    /* Bulk data from the PC: a header, then the JPEG picture, possibly over many calls */
#if CFG_TUD_API_V0_19_COMPAT
    void tud_vendor_rx_cb(uint8_t itf, const uint8_t *buffer, uint16_t bufsize)
#else
    void tud_vendor_rx_cb(uint8_t itf, const uint8_t *buffer, uint32_t bufsize)
#endif
    {
        (void)buffer;
        (void)bufsize;
        static uint8_t chunk[2048];
        while (tud_vendor_n_available(itf)) {
            if (!s_rx_active && !s_rx_skip && tud_vendor_n_available(itf) < sizeof(FrameHeader)) {
                break;
            }
            const uint32_t n = tud_vendor_n_read(itf, chunk, sizeof(chunk));
            if (n == 0) {
                break;
            }
            const uint8_t *data = chunk;
            uint32_t len = n;
            if (!s_rx_active && !s_rx_skip) {
                FrameHeader h;
                memcpy(&h, chunk, sizeof(h));
                data += sizeof(h);
                len -= sizeof(h);
                s_rx_total = h.payload_total;
                s_rx_got = 0;
                const bool ok = h.type == TYPE_JPG && h.x == 0 && h.y == 0 && h.width == s_host_w &&
                                h.height == s_host_h && s_rx_total > 0 && s_rx_total <= FRAME_MAX && s_in[s_rx_buf];
                if (!ok && h.type == TYPE_JPG) {
                    ESP_LOGW(TAG, "skip picture: x=%u y=%u %ux%u, %u bytes", h.x, h.y, h.width, h.height,
                             static_cast<unsigned>(s_rx_total));
                }
                s_rx_active = ok;
                s_rx_skip = !ok && s_rx_total > 0;
            }
            if (s_rx_active) {
                const uint32_t take = len < s_rx_total - s_rx_got ? len : s_rx_total - s_rx_got;
                memcpy(s_in[s_rx_buf] + s_rx_got, data, take);
                s_rx_got += take;
                if (s_rx_got >= s_rx_total) {
                    s_rx_active = false;
                    rx_frame_done();
                }
            } else if (s_rx_skip) {
                s_rx_got += len;
                if (s_rx_got >= s_rx_total) {
                    s_rx_skip = false;
                }
            }
        }
    }

}

namespace esp_brookesia::apps {

PhoneSecondScreen *PhoneSecondScreen::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneSecondScreen();
    }
    return _instance;
}

PhoneSecondScreen::PhoneSecondScreen():
    App(phone_tr("Màn hình phụ", "Second screen"), &phone_second_screen_icon, true, true, phone_nav_bar())
{
}

PhoneSecondScreen::~PhoneSecondScreen()
{
}

bool PhoneSecondScreen::run()
{
    s_landscape = phone_pref_get("ss_land", 0) != 0;
    build_ui();
    lv_timer_create(on_timer, 10, nullptr);
    return true;
}

bool PhoneSecondScreen::back()
{
    if (s_running) {
        stop_screen();
        return true;
    }
    return notifyCoreClosed();
}

bool PhoneSecondScreen::close()
{
    stop_screen();
    return true;
}

bool PhoneSecondScreen::pause()
{
    stop_screen();
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneSecondScreen, APP_NAME, []()
{
    return std::shared_ptr<PhoneSecondScreen>(PhoneSecondScreen::requestInstance(), [](PhoneSecondScreen * p) {});
})

} // namespace esp_brookesia::apps
