/*
 * USB camera app: live picture of a UVC webcam plugged into the USB host (Type-A) socket, and
 * photos saved on the microSD card.
 *
 * The app turns the 5 V boost on (VBUS of the Type-A socket), installs the USB host library on
 * the high-speed port and the UVC class driver, and opens the first MJPEG format of the webcam
 * that fits. Every frame is a JPEG picture: decoded by the hardware decoder, scaled (and turned,
 * on request) by the PPA into one of two buffers shown by an LVGL image. A photo is the JPEG
 * frame as the webcam sent it.
 *
 * The USB HS port is shared with the Type-C HS socket and with the Second screen app (USB
 * device): leave the Type-C HS socket unplugged while a webcam is in use.
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "USBCAM"
#include "esp_lib_utils.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"
#include "phone_storage.h"
#include "phone_app_usb_camera.hpp"

#define APP_NAME "Camera USB"

extern "C" const lv_image_dsc_t phone_usb_camera_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_usbcam";

constexpr int ALIGN = 128;                  // L2 cache line, required for PPA output buffers
constexpr int CONTROLS_H = 150;             // bar under the picture
constexpr int FRAME_BUFS = 3;
constexpr size_t FRAME_SIZE = 512 * 1024;   // one MJPEG frame of the webcam
constexpr int MAX_W = 1280;                 // largest format asked from the webcam
constexpr int MAX_H = 720;
constexpr int FORMATS_MAX = 48;
constexpr const char *PHOTO_DIR = "/sdcard/DCIM";

enum State { ST_OFF, ST_WAITING, ST_OPENING, ST_STREAMING, ST_ERROR };
enum Shot { SHOT_NONE, SHOT_REQUESTED, SHOT_SAVED, SHOT_FAILED, SHOT_NO_CARD };

lv_obj_t *s_root;
lv_obj_t *s_img;
lv_obj_t *s_status;
lv_obj_t *s_shutter;
int s_area_w, s_area_h;
uint8_t *s_out[2];
size_t s_out_size;
lv_image_dsc_t s_dsc[2];
int s_shown_state = -1;
uint32_t s_msg_until;

TaskHandle_t s_worker;
TaskHandle_t s_usb_task;
SemaphoreHandle_t s_worker_done;
SemaphoreHandle_t s_usb_done;
QueueHandle_t s_frames;                     // uvc_host_frame_t *, from the frame callback
std::atomic<bool> s_stop;
std::atomic<bool> s_usb_stop;
std::atomic<bool> s_connected;              // a UVC device is waiting to be opened
std::atomic<bool> s_disconnected;           // the open stream lost its device
std::atomic<int> s_dev_addr;
std::atomic<int> s_stream_index;
std::atomic<int> s_state{ST_OFF};
std::atomic<int> s_shot{SHOT_NONE};
std::atomic<bool> s_rotate;                 // landscape picture turned to fill the screen
std::atomic<int> s_res_w, s_res_h;
char s_shot_name[40];
uvc_host_stream_hdl_t s_stream;             // worker task only

size_t align_up(size_t n)
{
    return (n + ALIGN - 1) & ~static_cast<size_t>(ALIGN - 1);
}

void fill_dsc(lv_image_dsc_t &dsc, const uint8_t *data, int w, int h)
{
    dsc = {};
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = w;
    dsc.header.h = h;
    dsc.header.stride = w * 2;
    dsc.data_size = w * h * 2;
    dsc.data = data;
}

/* ---- USB host ---- */

void usb_lib_task(void *)
{
    bool all_free = false;
    while (!s_usb_stop || !all_free) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            /* ESP_OK: nothing left to free; otherwise ALL_FREE follows */
            all_free = usb_host_device_free_all() == ESP_OK;
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            all_free = true;
        }
    }
    xSemaphoreGive(s_usb_done);
    vTaskDelete(nullptr);
}

void uvc_driver_event(const uvc_host_driver_event_data_t *event, void *)
{
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        s_dev_addr = event->device_connected.dev_addr;
        s_stream_index = event->device_connected.uvc_stream_index;
        s_connected = true;
    }
}

void uvc_stream_event(const uvc_host_stream_event_data_t *event, void *)
{
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        s_disconnected = true;
    }
}

/* The frame goes to the worker; it comes back with uvc_host_frame_return() */
bool uvc_frame(const uvc_host_frame_t *frame, void *)
{
    return xQueueSend(s_frames, &frame, 0) != pdPASS;   /* queue full: the driver keeps the frame */
}

/* ---- worker ---- */

/* Largest MJPEG format of the webcam within MAX_W x MAX_H; false when it has none */
bool pick_format(uvc_host_stream_format_t &fmt)
{
    static uvc_host_frame_info_t list[FORMATS_MAX];
    size_t n = FORMATS_MAX;
    if (uvc_host_get_frame_list(s_dev_addr, s_stream_index, reinterpret_cast<uvc_host_frame_info_t (*)[]>(&list),
                                &n) != ESP_OK) {
        return false;
    }
    int best = -1;
    for (size_t i = 0; i < n && i < FORMATS_MAX; i++) {
        if (list[i].format != UVC_VS_FORMAT_MJPEG || list[i].h_res > MAX_W || list[i].v_res > MAX_H) {
            continue;
        }
        if (best < 0 || list[i].h_res * list[i].v_res > list[best].h_res * list[best].v_res) {
            best = static_cast<int>(i);
        }
    }
    if (best < 0) {
        return false;
    }
    fmt = {};
    fmt.h_res = list[best].h_res;
    fmt.v_res = list[best].v_res;
    fmt.fps = 0;            /* the default of the webcam */
    fmt.format = UVC_VS_FORMAT_MJPEG;
    return true;
}

void photo_path(char *path, size_t size)
{
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    struct stat st;
    if (tm.tm_year + 1900 >= 2024) {
        char stamp[24];
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
        snprintf(path, size, "%s/USB_%s.jpg", PHOTO_DIR, stamp);
        for (int n = 2; stat(path, &st) == 0 && n < 100; n++) {
            snprintf(path, size, "%s/USB_%s_%d.jpg", PHOTO_DIR, stamp, n);
        }
        return;
    }
    int n = phone_pref_get("ucam_n", 0);
    do {
        snprintf(path, size, "%s/USB_%04d.jpg", PHOTO_DIR, ++n);
    } while (stat(path, &st) == 0 && n < 9999);
    phone_pref_set("ucam_n", n);
}

/* The JPEG frame, as the webcam sent it */
int save_photo(const uvc_host_frame_t *frame)
{
    if (phone_storage_get(nullptr, nullptr) != PHONE_STORAGE_MOUNTED) {
        return SHOT_NO_CARD;
    }
    mkdir(PHOTO_DIR, 0775);
    char path[64];
    photo_path(path, sizeof(path));
    FILE *file = fopen(path, "wb");
    if (!file) {
        return SHOT_FAILED;
    }
    const bool ok = fwrite(frame->data, 1, frame->data_len, file) == frame->data_len;
    if (fclose(file) != 0 || !ok) {
        unlink(path);
        return SHOT_FAILED;
    }
    strlcpy(s_shot_name, strrchr(path, '/') + 1, sizeof(s_shot_name));
    ESP_LOGI(TAG, "saved %s (%u bytes)", path, static_cast<unsigned>(frame->data_len));
    return SHOT_SAVED;
}

void worker(void *)
{
    jpeg_decoder_handle_t dec = nullptr;
    ppa_client_handle_t ppa = nullptr;
    uint8_t *rgb = nullptr;
    size_t rgb_size = 0;
    int rgb_w = 0, rgb_h = 0;
    uint8_t *frame_bufs[FRAME_BUFS] = {};
    size_t frame_buf_size = 0;
    int back = 0, bad = 0;
    const jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    const jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    const jpeg_decode_engine_cfg_t eng_cfg = {.intr_priority = 0, .timeout_ms = 500};
    const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};
    const jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    bool ok = jpeg_new_decoder_engine(&eng_cfg, &dec) == ESP_OK && ppa_register_client(&ppa_cfg, &ppa) == ESP_OK;
    /* the hardware decoder reads the frames in place: give the driver aligned buffers */
    for (int i = 0; ok && i < FRAME_BUFS; i++) {
        frame_bufs[i] = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(FRAME_SIZE, &in_cfg, &frame_buf_size));
        ok = frame_bufs[i] != nullptr;
    }
    s_state = ok ? ST_WAITING : ST_ERROR;

    while (ok && !s_stop) {
        if (!s_stream) {
            if (!s_connected.exchange(false)) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            s_state = ST_OPENING;
            uvc_host_stream_config_t cfg = {};
            cfg.event_cb = uvc_stream_event;
            cfg.frame_cb = uvc_frame;
            cfg.usb.dev_addr = static_cast<uint8_t>(s_dev_addr.load());
            cfg.usb.uvc_stream_index = static_cast<uint8_t>(s_stream_index.load());
            cfg.advanced.number_of_frame_buffers = FRAME_BUFS;
            cfg.advanced.frame_size = FRAME_SIZE;
            cfg.advanced.number_of_urbs = 4;
            cfg.advanced.urb_size = 10 * 1024;
            cfg.advanced.user_frame_buffers = frame_bufs;
            uvc_host_stream_format_t fmt = {};
            bool opened = false;
            if (pick_format(fmt)) {
                memcpy(const_cast<uvc_host_stream_format_t *>(&cfg.vs_format), &fmt, sizeof(fmt));
                s_disconnected = false;
                opened = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(3000), &s_stream) == ESP_OK &&
                         uvc_host_stream_start(s_stream) == ESP_OK;
            }
            if (opened) {
                s_res_w = fmt.h_res;
                s_res_h = fmt.v_res;
                bad = 0;
                ESP_LOGI(TAG, "webcam: %u x %u MJPEG", fmt.h_res, fmt.v_res);
                s_state = ST_STREAMING;
            } else {
                ESP_LOGE(TAG, "cannot open the webcam (no MJPEG format up to %d x %d?)", MAX_W, MAX_H);
                if (s_stream) {
                    uvc_host_stream_close(s_stream);
                    s_stream = nullptr;
                }
                s_state = ST_ERROR;
            }
            continue;
        }

        uvc_host_frame_t *frame = nullptr;
        if (xQueueReceive(s_frames, &frame, pdMS_TO_TICKS(100)) == pdPASS) {
            if (s_shot == SHOT_REQUESTED) {
                s_shot = save_photo(frame);
            }
            jpeg_decode_picture_info_t info = {};
            bool shown = false;
            if (!phone_lock_is_locked() && jpeg_decoder_get_info(frame->data, frame->data_len, &info) == ESP_OK &&
                    info.width > 0 && info.height > 0 && info.width <= 1920 && info.height <= 1088) {
                const int w = info.width, h = info.height;
                const int buf_w = (w + 15) & ~15, buf_h = (h + 15) & ~15;
                if (buf_w != rgb_w || buf_h != rgb_h) {
                    free(rgb);
                    rgb = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(static_cast<size_t>(buf_w) * buf_h * 2, &out_cfg,
                                                 &rgb_size));
                    rgb_w = rgb ? buf_w : 0;
                    rgb_h = rgb ? buf_h : 0;
                }
                uint32_t len = 0;
                if (rgb && jpeg_decoder_process(dec, &dec_cfg, frame->data, frame->data_len, rgb, rgb_size, &len) == ESP_OK) {
                    /* fit the room for the picture in 1/16 steps */
                    const bool turned = s_rotate && w > h;
                    const int rw = turned ? h : w;
                    const int rh = turned ? w : h;
                    int steps = s_area_w * 16 / rw < s_area_h * 16 / rh ? s_area_w * 16 / rw : s_area_h * 16 / rh;
                    steps = steps < 1 ? 1 : (steps > 48 ? 48 : steps);
                    const int out_w = rw * steps / 16, out_h = rh * steps / 16;
                    ppa_srm_oper_config_t op = {};
                    op.in.buffer = rgb;
                    op.in.pic_w = buf_w;
                    op.in.pic_h = buf_h;
                    op.in.block_w = w;
                    op.in.block_h = h;
                    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
                    op.out.buffer = s_out[back];
                    op.out.buffer_size = s_out_size;
                    op.out.pic_w = out_w;
                    op.out.pic_h = out_h;
                    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
                    op.rotation_angle = turned ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_0;
                    op.scale_x = steps / 16.0f;
                    op.scale_y = steps / 16.0f;
                    op.mode = PPA_TRANS_MODE_BLOCKING;
                    if (ppa_do_scale_rotate_mirror(ppa, &op) == ESP_OK && bsp_display_lock(50)) {
                        if (s_img && !s_stop) {
                            fill_dsc(s_dsc[back], s_out[back], out_w, out_h);
                            lv_image_set_src(s_img, &s_dsc[back]);
                            back ^= 1;
                        }
                        bsp_display_unlock();
                    }
                    shown = true;
                }
            }
            if (!shown && !phone_lock_is_locked() && ++bad == 30) {
                ESP_LOGW(TAG, "frames cannot be decoded (%u bytes)", static_cast<unsigned>(frame->data_len));
            } else if (shown) {
                bad = 0;
            }
            uvc_host_frame_return(s_stream, frame);
        }

        if (s_disconnected.exchange(false)) {
            ESP_LOGI(TAG, "webcam unplugged");
            while (xQueueReceive(s_frames, &frame, 0) == pdPASS) {
                uvc_host_frame_return(s_stream, frame);
            }
            uvc_host_stream_close(s_stream);
            s_stream = nullptr;
            s_state = ST_WAITING;
        }
    }

    if (s_stream) {
        uvc_host_frame_t *frame = nullptr;
        uvc_host_stream_stop(s_stream);
        while (xQueueReceive(s_frames, &frame, 0) == pdPASS) {
            uvc_host_frame_return(s_stream, frame);
        }
        uvc_host_stream_close(s_stream);
        s_stream = nullptr;
    }
    for (auto &buf : frame_bufs) {
        free(buf);
    }
    free(rgb);
    if (ppa) {
        ppa_unregister_client(ppa);
    }
    if (dec) {
        jpeg_del_decoder_engine(dec);
    }
    xSemaphoreGive(s_worker_done);
    vTaskDelete(nullptr);
}

/* ---- start / stop ---- */

bool s_host_installed;
bool s_uvc_installed;

void stop_camera()
{
    if (s_worker) {
        s_stop = true;
        if (xSemaphoreTake(s_worker_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "worker did not stop");
        }
        s_worker = nullptr;
    }
    if (s_uvc_installed) {
        uvc_host_uninstall();
        s_uvc_installed = false;
    }
    if (s_usb_task) {
        s_usb_stop = true;
        if (xSemaphoreTake(s_usb_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGE(TAG, "USB task did not stop");
        }
        s_usb_task = nullptr;
    }
    if (s_host_installed) {
        if (usb_host_uninstall() != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_uninstall failed");
        }
        s_host_installed = false;
    }
    bsp_power_set(BSP_POWER_BOOST_5V, false);
    s_state = ST_OFF;
}

void start_camera()
{
    if (s_worker || s_host_installed) {
        return;
    }
    if (!s_frames) {
        s_frames = xQueueCreate(FRAME_BUFS, sizeof(uvc_host_frame_t *));
        s_worker_done = xSemaphoreCreateBinary();
        s_usb_done = xSemaphoreCreateBinary();
    }
    xQueueReset(s_frames);
    s_stop = false;
    s_usb_stop = false;
    s_connected = false;
    s_disconnected = false;
    s_shot = SHOT_NONE;
    s_state = ST_ERROR;

    /* VBUS of the Type-A socket */
    bsp_power_set(BSP_POWER_BOOST_5V, true);
    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags = ESP_INTR_FLAG_LOWMED;
    if (usb_host_install(&host_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed");
        bsp_power_set(BSP_POWER_BOOST_5V, false);
        return;
    }
    s_host_installed = true;
    if (xTaskCreate(usb_lib_task, "usbcam_usb", 4096, nullptr, 10, &s_usb_task) != pdPASS) {
        s_usb_task = nullptr;
        stop_camera();
        s_state = ST_ERROR;
        return;
    }
    uvc_host_driver_config_t uvc_cfg = {};
    uvc_cfg.driver_task_stack_size = 4096;
    uvc_cfg.driver_task_priority = 11;
    uvc_cfg.xCoreID = tskNO_AFFINITY;
    uvc_cfg.create_background_task = true;
    uvc_cfg.event_cb = uvc_driver_event;
    if (uvc_host_install(&uvc_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_install failed");
        stop_camera();
        s_state = ST_ERROR;
        return;
    }
    s_uvc_installed = true;
    if (xTaskCreate(worker, "usbcam", 8192, nullptr, 4, &s_worker) != pdPASS) {
        s_worker = nullptr;
        stop_camera();
        s_state = ST_ERROR;
    }
}

/* ---- UI ---- */

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
    const int shot = s_shot;
    if (shot == SHOT_SAVED || shot == SHOT_FAILED || shot == SHOT_NO_CARD) {
        s_shot = SHOT_NONE;
        if (shot == SHOT_SAVED) {
            lv_label_set_text_fmt(s_status, "%s %s", T("Đã lưu", "Saved"), s_shot_name);
            lv_obj_set_style_text_color(s_status, lv_color_hex(0x22C55E), 0);
        } else if (shot == SHOT_NO_CARD) {
            set_status(T("Không có thẻ nhớ", "No microSD card"), 0xF87171);
        } else {
            set_status(T("Không lưu được ảnh", "Could not save the photo"), 0xF87171);
        }
        lv_obj_remove_state(s_shutter, LV_STATE_DISABLED);
        s_msg_until = lv_tick_get() + 3000;
        s_shown_state = -1;
    } else if (shot == SHOT_NONE && state != s_shown_state && lv_tick_get() >= s_msg_until) {
        s_shown_state = state;
        switch (state) {
        case ST_WAITING:
            set_status(T("Cắm webcam USB vào cổng Type-A", "Plug a USB webcam into the Type-A socket"), 0x9AA5B1);
            lv_image_set_src(s_img, nullptr);
            break;
        case ST_OPENING:
            set_status(T("Đang mở webcam...", "Opening the webcam..."), 0x9AA5B1);
            break;
        case ST_STREAMING:
            lv_label_set_text_fmt(s_status, "%d x %d MJPEG", s_res_w.load(), s_res_h.load());
            lv_obj_set_style_text_color(s_status, lv_color_hex(0x9AA5B1), 0);
            break;
        case ST_ERROR:
            set_status(T("Không mở được webcam (cần định dạng MJPEG)", "Cannot open the webcam (MJPEG needed)"), 0xF87171);
            break;
        default:
            set_status("", 0x9AA5B1);
            break;
        }
    }
    if (state == ST_STREAMING && !phone_lock_is_locked() && lv_obj_get_screen(s_root) == lv_screen_active()) {
        lv_display_trigger_activity(nullptr);       /* being watched: keep the screen on */
    }
}

void on_shutter(lv_event_t *)
{
    if (s_state != ST_STREAMING || s_shot != SHOT_NONE) {
        return;
    }
    s_shot = SHOT_REQUESTED;
    lv_obj_add_state(s_shutter, LV_STATE_DISABLED);
    set_status(T("Đang lưu...", "Saving..."), 0x9AA5B1);
}

void on_rotate(lv_event_t *)
{
    s_rotate = !s_rotate;
    phone_pref_set("ucam_rot", s_rotate ? 1 : 0);
}

void build_ui()
{
    s_shown_state = -1;
    s_msg_until = 0;

    lv_obj_t *root = lv_obj_create(lv_screen_active());
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_text_color(root, lv_color_white(), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(18), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
        s_img = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    const int root_w = lv_obj_get_width(root);
    const int root_h = lv_obj_get_height(root);
    s_area_w = root_w;
    s_area_h = root_h - CONTROLS_H;

    /* picture: centered in the room above the controls */
    lv_obj_t *area = lv_obj_create(root);
    lv_obj_remove_style_all(area);
    lv_obj_set_size(area, s_area_w, s_area_h);
    lv_obj_remove_flag(area, LV_OBJ_FLAG_SCROLLABLE);
    s_img = lv_image_create(area);
    lv_obj_center(s_img);

    s_status = lv_label_create(root);
    lv_obj_set_width(s_status, root_w - 24);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, s_area_h + 4);
    lv_label_set_text(s_status, "");

    /* shutter: white disc in a ring */
    lv_obj_t *ring = lv_obj_create(root);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 80, 80);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 4, 0);
    lv_obj_set_style_border_color(ring, lv_color_white(), 0);
    lv_obj_align(ring, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_remove_flag(ring, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    s_shutter = lv_button_create(ring);
    lv_obj_remove_style_all(s_shutter);
    lv_obj_set_size(s_shutter, 60, 60);
    lv_obj_center(s_shutter);
    lv_obj_set_ext_click_area(s_shutter, 14);
    lv_obj_set_style_radius(s_shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_shutter, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_shutter, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_shutter, lv_color_hex(0xB0B8C0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_shutter, lv_color_hex(0x5A6470), LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_shutter, on_shutter, LV_EVENT_CLICKED, nullptr);

    /* turn a landscape picture to fill the screen */
    lv_obj_t *rot = lv_button_create(root);
    lv_obj_remove_style_all(rot);
    lv_obj_set_size(rot, 56, 56);
    lv_obj_set_style_radius(rot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(rot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rot, lv_color_hex(0x2A333C), 0);
    lv_obj_set_style_bg_color(rot, lv_color_hex(0x4A5560), LV_STATE_PRESSED);
    lv_obj_align(rot, LV_ALIGN_BOTTOM_RIGHT, -28, -42);
    lv_obj_add_event_cb(rot, on_rotate, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *rl = lv_label_create(rot);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_24, 0);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_center(rl);
}

bool alloc_out()
{
    /* room for the picture in either orientation */
    const int side = s_area_w > s_area_h ? s_area_w : s_area_h;
    s_out_size = align_up(static_cast<size_t>(s_area_w) * side * 2);
    for (auto &buf : s_out) {
        if (!buf) {
            buf = static_cast<uint8_t *>(heap_caps_aligned_calloc(ALIGN, 1, s_out_size, MALLOC_CAP_SPIRAM));
        }
        if (!buf) {
            return false;
        }
    }
    return true;
}

void free_out()
{
    for (auto &buf : s_out) {
        heap_caps_free(buf);
        buf = nullptr;
    }
}

} // namespace

namespace esp_brookesia::apps {

PhoneUsbCamera *PhoneUsbCamera::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneUsbCamera();
    }
    return _instance;
}

PhoneUsbCamera::PhoneUsbCamera():
    App(phone_tr("Camera USB", "USB camera"), &phone_usb_camera_icon, true, true, true)
{
}

PhoneUsbCamera::~PhoneUsbCamera()
{
}

bool PhoneUsbCamera::run()
{
    s_rotate = phone_pref_get("ucam_rot", 0) != 0;
    build_ui();
    if (!alloc_out()) {
        ESP_LOGE(TAG, "no memory for the picture");
        free_out();
        return false;
    }
    lv_timer_create(on_timer, 100, nullptr);
    start_camera();
    return true;
}

bool PhoneUsbCamera::back()
{
    return notifyCoreClosed();
}

bool PhoneUsbCamera::close()
{
    stop_camera();
    /* the image still points at a picture buffer until the core deletes the screen */
    if (s_img) {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_img, nullptr);
    }
    free_out();
    return true;
}

/* In the background: the USB port and the 5 V boost are released */
bool PhoneUsbCamera::pause()
{
    stop_camera();
    return true;
}

bool PhoneUsbCamera::resume()
{
    start_camera();
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneUsbCamera, APP_NAME, []()
{
    return std::shared_ptr<PhoneUsbCamera>(PhoneUsbCamera::requestInstance(), [](PhoneUsbCamera * p) {});
})

} // namespace esp_brookesia::apps
