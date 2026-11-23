/*
 * Camera app: live preview of the MIPI CSI sensor and photos saved as JPEG on the microSD card.
 *
 * A worker task reads RGB565 frames from the V4L2 device. Each frame is cropped, scaled and
 * rotated by the PPA into one of two preview buffers, which the LVGL image then shows. A photo is
 * the full frame, rotated by the PPA and compressed by the hardware JPEG encoder.
 *
 * The worker runs on core 0: the CSI and DSI controllers share the DW-GDMA interrupt, and a
 * shared interrupt can only be attached on the core that first allocated it (the display).
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "phone_paths.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "CAM"
#include "esp_lib_utils.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"
#include "phone_storage.h"
#include "phone_app_camera.hpp"

#define APP_NAME "Máy ảnh"

extern "C" const lv_image_dsc_t phone_camera_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_cam";

constexpr int PREVIEW_W = 480;              // preview area on the screen (portrait, 3:4)
constexpr int PREVIEW_H = 640;
constexpr int BUFS = 2;                     // V4L2 frame buffers
constexpr int ALIGN = 128;                  // L2 cache line, required for PPA output buffers
constexpr int JPEG_QUALITY = 90;
constexpr const char *PHOTO_DIR = PHONE_SD_DCIM;
/* How the sensor image is turned to stand upright on the portrait screen (counter-clockwise) */
constexpr ppa_srm_rotation_angle_t ROTATION = PPA_SRM_ROTATION_ANGLE_90;
constexpr bool MIRROR_X = false;

enum State { ST_STARTING, ST_RUNNING, ST_ERROR, ST_STOPPED };
enum Shot { SHOT_NONE, SHOT_REQUESTED, SHOT_SAVED, SHOT_FAILED, SHOT_NO_CARD };

lv_obj_t *s_root;
lv_obj_t *s_img;
lv_obj_t *s_status;
lv_obj_t *s_shutter;
lv_image_dsc_t s_dsc[2];
uint8_t *s_preview[2];
size_t s_preview_size;

TaskHandle_t s_task;
SemaphoreHandle_t s_done;
std::atomic<bool> s_stop;
std::atomic<int> s_state{ST_STOPPED};
std::atomic<int> s_shot{SHOT_NONE};
char s_shot_name[40];
int s_shown_state = -1;
uint32_t s_msg_until;           // LVGL tick until which the last photo message stays

struct Frame {
    int w, h;                   // sensor frame
    int block_w, block_h;       // cropped source block for the preview
    float scale;
};

/* Crop and scale so that the rotated block fills the preview; the PPA scales in 1/16 steps */
void plan_preview(Frame &f)
{
    const bool turned = ROTATION == PPA_SRM_ROTATION_ANGLE_90 || ROTATION == PPA_SRM_ROTATION_ANGLE_270;
    const int out_w = turned ? PREVIEW_H : PREVIEW_W;      // size of the block before rotation
    const int out_h = turned ? PREVIEW_W : PREVIEW_H;
    /* largest block with the preview aspect ratio that fits in the frame */
    int crop_w = f.w;
    int crop_h = crop_w * out_h / out_w;
    if (crop_h > f.h) {
        crop_h = f.h;
        crop_w = crop_h * out_w / out_h;
    }
    int steps = (out_w * 16 + crop_w - 1) / crop_w;         // round the scale up, then crop a bit more
    if (steps < 1) {
        steps = 1;
    }
    f.scale = steps / 16.0f;
    f.block_w = (out_w * 16 / steps) & ~1;
    f.block_h = (out_h * 16 / steps) & ~1;
    if (f.block_w > f.w) {
        f.block_w = f.w & ~1;
    }
    if (f.block_h > f.h) {
        f.block_h = f.h & ~1;
    }
}

bool render_preview(ppa_client_handle_t ppa, const Frame &f, const void *frame, uint8_t *out)
{
    ppa_srm_oper_config_t op = {};
    op.in.buffer = frame;
    op.in.pic_w = f.w;
    op.in.pic_h = f.h;
    op.in.block_w = f.block_w;
    op.in.block_h = f.block_h;
    op.in.block_offset_x = (f.w - f.block_w) / 2;
    op.in.block_offset_y = (f.h - f.block_h) / 2;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = out;
    op.out.buffer_size = s_preview_size;
    op.out.pic_w = PREVIEW_W;
    op.out.pic_h = PREVIEW_H;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = ROTATION;
    op.scale_x = f.scale;
    op.scale_y = f.scale;
    op.mirror_x = MIRROR_X;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(ppa, &op) == ESP_OK;
}

/* Next free photo path: named after the clock when it is set, numbered otherwise */
void photo_path(char *path, size_t size)
{
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2024) {
        char stamp[24];
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
        snprintf(path, size, "%s/IMG_%s.jpg", PHOTO_DIR, stamp);
        struct stat st;
        for (int n = 2; stat(path, &st) == 0 && n < 100; n++) {
            snprintf(path, size, "%s/IMG_%s_%d.jpg", PHOTO_DIR, stamp, n);
        }
        return;
    }
    int n = phone_pref_get("cam_n", 0);
    struct stat st;
    do {
        snprintf(path, size, "%s/IMG_%04d.jpg", PHOTO_DIR, ++n);
    } while (stat(path, &st) == 0 && n < 9999);
    phone_pref_set("cam_n", n);
}

/* Full frame, rotated like the preview, JPEG encoded and written to the card */
int take_photo(ppa_client_handle_t ppa, const Frame &f, const void *frame)
{
    if (phone_storage_get(nullptr, nullptr) != PHONE_STORAGE_MOUNTED) {
        return SHOT_NO_CARD;
    }
    const bool turned = ROTATION == PPA_SRM_ROTATION_ANGLE_90 || ROTATION == PPA_SRM_ROTATION_ANGLE_270;
    const int w = turned ? f.h : f.w;
    const int h = turned ? f.w : f.h;
    int result = SHOT_FAILED;
    jpeg_encoder_handle_t enc = nullptr;
    size_t raw_size = 0, jpg_size = 0;
    const jpeg_encode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
    const jpeg_encode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    auto *raw = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(static_cast<size_t>(w) * h * 2, &in_cfg, &raw_size));
    auto *jpg = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(static_cast<size_t>(w) * h, &out_cfg, &jpg_size));
    const jpeg_encode_engine_cfg_t eng_cfg = {.intr_priority = 0, .timeout_ms = 2000};

    if (raw && jpg && jpeg_new_encoder_engine(&eng_cfg, &enc) == ESP_OK) {
        ppa_srm_oper_config_t op = {};
        op.in.buffer = frame;
        op.in.pic_w = f.w;
        op.in.pic_h = f.h;
        op.in.block_w = f.w;
        op.in.block_h = f.h;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = raw;
        op.out.buffer_size = raw_size;
        op.out.pic_w = w;
        op.out.pic_h = h;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = ROTATION;
        op.scale_x = 1.0f;
        op.scale_y = 1.0f;
        op.mirror_x = MIRROR_X;
        op.mode = PPA_TRANS_MODE_BLOCKING;

        jpeg_encode_cfg_t cfg = {};
        cfg.width = w;
        cfg.height = h;
        cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
        cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
        cfg.image_quality = JPEG_QUALITY;
        uint32_t len = 0;
        if (ppa_do_scale_rotate_mirror(ppa, &op) == ESP_OK &&
                jpeg_encoder_process(enc, &cfg, raw, static_cast<uint32_t>(w) * h * 2, jpg, jpg_size, &len) == ESP_OK) {
            mkdir(PHOTO_DIR, 0775);
            char path[64];
            photo_path(path, sizeof(path));
            FILE *file = fopen(path, "wb");
            if (file) {
                const bool ok = fwrite(jpg, 1, len, file) == len;
                if (fclose(file) == 0 && ok) {
                    strlcpy(s_shot_name, strrchr(path, '/') + 1, sizeof(s_shot_name));
                    ESP_LOGI(TAG, "saved %s (%lu bytes, %d x %d)", path, static_cast<unsigned long>(len), w, h);
                    result = SHOT_SAVED;
                } else {
                    unlink(path);
                }
            }
            if (result != SHOT_SAVED) {
                ESP_LOGE(TAG, "cannot write %s", path);
            }
        } else {
            ESP_LOGE(TAG, "rotate or JPEG encode failed");
        }
    } else {
        ESP_LOGE(TAG, "no memory or no JPEG encoder for a %d x %d photo", w, h);
    }
    if (enc) {
        jpeg_del_encoder_engine(enc);
    }
    free(raw);
    free(jpg);
    return result;
}

/* One camera session: sensor on, frames until the app stops it or the phone locks, sensor off.
 * Returns false when the camera failed. */
bool camera_session()
{
    int fd = -1;
    void *map[BUFS] = {};
    size_t len[BUFS] = {};
    ppa_client_handle_t ppa = nullptr;
    bool streaming = false;
    Frame f = {};
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int back = 0;
    const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};

    bool ok = bsp_camera_start(nullptr) == ESP_OK;
    if (ok) {
        fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
        struct v4l2_format fmt = {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ok = fd >= 0 && ioctl(fd, VIDIOC_G_FMT, &fmt) == 0;
        if (ok) {
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
            ok = ioctl(fd, VIDIOC_S_FMT, &fmt) == 0 && fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565;
            f.w = fmt.fmt.pix.width;
            f.h = fmt.fmt.pix.height;
        }
        struct v4l2_requestbuffers req = {};
        req.count = BUFS;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        ok = ok && ioctl(fd, VIDIOC_REQBUFS, &req) == 0;
        for (int i = 0; ok && i < BUFS; i++) {
            struct v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            ok = ioctl(fd, VIDIOC_QUERYBUF, &buf) == 0;
            if (ok) {
                void *p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
                ok = p != MAP_FAILED && ioctl(fd, VIDIOC_QBUF, &buf) == 0;
                if (p != MAP_FAILED) {
                    map[i] = p;
                    len[i] = buf.length;
                }
            }
        }
        ok = ok && ppa_register_client(&ppa_cfg, &ppa) == ESP_OK;
        ok = ok && ioctl(fd, VIDIOC_STREAMON, &type) == 0;
        streaming = ok;
    }
    if (ok) {
        plan_preview(f);
        ESP_LOGI(TAG, "sensor %d x %d, preview block %d x %d, scale %.4f", f.w, f.h, f.block_w, f.block_h, f.scale);
        s_state = ST_RUNNING;
    } else {
        ESP_LOGE(TAG, "camera start failed");
        s_state = ST_ERROR;
    }

    bool failed = !ok;
    while (ok && !s_stop && !phone_lock_is_locked()) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "frame read failed");
            s_state = ST_ERROR;
            failed = true;
            break;
        }
        const void *frame = map[buf.index];
        if (s_shot == SHOT_REQUESTED) {
            s_shot = take_photo(ppa, f, frame);
        }
        if (render_preview(ppa, f, frame, s_preview[back])) {
            if (bsp_display_lock(50)) {
                if (s_img && !s_stop) {
                    lv_image_set_src(s_img, &s_dsc[back]);
                    back ^= 1;
                }
                bsp_display_unlock();
            }
        }
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    if (streaming) {
        ioctl(fd, VIDIOC_STREAMOFF, &type);
    }
    for (int i = 0; i < BUFS; i++) {
        if (map[i]) {
            munmap(map[i], len[i]);
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    if (ppa) {
        ppa_unregister_client(ppa);
    }
    bsp_camera_stop();
    return !failed;
}

/* While the lock screen is up or the screen is off nothing is seen: the sensor is off (core 0 stayed near 50 %
 * behind the lock screen), and on again once unlocked. Runs here rather than in an LVGL timer: LVGL is paused
 * while the screen is off. */
void worker(void *)
{
    while (!s_stop) {
        if (phone_lock_is_locked()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        s_state = ST_STARTING;
        if (!camera_session()) {
            break;
        }
        if (!s_stop) {
            ESP_LOGI(TAG, "locked: camera off until unlocked");
        }
    }
    if (s_state != ST_ERROR) {
        s_state = ST_STOPPED;
    }
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

void start_worker()
{
    if (s_task) {
        return;
    }
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
    }
    s_stop = false;
    s_shot = SHOT_NONE;
    s_state = ST_STARTING;
    if (xTaskCreatePinnedToCore(worker, "phone_cam", 8192, nullptr, 4, &s_task, 0) != pdPASS) {
        s_task = nullptr;
        s_state = ST_ERROR;
    }
}

/* Called with the LVGL lock held: the worker never waits for that lock longer than 50 ms */
void stop_worker()
{
    if (!s_task) {
        return;
    }
    s_stop = true;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "camera task did not stop");
    }
    s_task = nullptr;
}

void set_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
    /* no text: no empty pill over the preview */
    lv_obj_set_flag(s_status, LV_OBJ_FLAG_HIDDEN, text[0] == '\0');
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
            lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
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
        if (state == ST_STARTING) {
            set_status(T("Đang mở máy ảnh...", "Starting the camera..."), 0x9AA5B1);
        } else if (state == ST_ERROR) {
            set_status(T("Không mở được máy ảnh", "Camera not available"), 0xF87171);
        } else {
            set_status("", 0x9AA5B1);
        }
    }
    /* The preview is being watched: keep the screen on */
    if (state == ST_RUNNING && !phone_lock_is_locked() && lv_obj_get_screen(s_root) == lv_screen_active()) {
        lv_display_trigger_activity(nullptr);
    }
}

void on_shutter(lv_event_t *)
{
    if (s_state != ST_RUNNING || s_shot != SHOT_NONE) {
        return;
    }
    s_shot = SHOT_REQUESTED;
    lv_obj_add_state(s_shutter, LV_STATE_DISABLED);
    set_status(T("Đang lưu...", "Saving..."), 0x9AA5B1);
}

bool alloc_preview()
{
    s_preview_size = (static_cast<size_t>(PREVIEW_W) * PREVIEW_H * 2 + ALIGN - 1) & ~static_cast<size_t>(ALIGN - 1);
    for (int i = 0; i < 2; i++) {
        if (!s_preview[i]) {
            s_preview[i] = static_cast<uint8_t *>(heap_caps_aligned_calloc(ALIGN, 1, s_preview_size, MALLOC_CAP_SPIRAM));
        }
        if (!s_preview[i]) {
            return false;
        }
        memset(s_preview[i], 0, s_preview_size);
        s_dsc[i] = {};
        s_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_dsc[i].header.w = PREVIEW_W;
        s_dsc[i].header.h = PREVIEW_H;
        s_dsc[i].header.stride = PREVIEW_W * 2;
        s_dsc[i].data_size = PREVIEW_W * PREVIEW_H * 2;
        s_dsc[i].data = s_preview[i];
    }
    return true;
}

void free_preview()
{
    for (int i = 0; i < 2; i++) {
        heap_caps_free(s_preview[i]);
        s_preview[i] = nullptr;
    }
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

    s_img = lv_image_create(root);
    lv_obj_set_size(s_img, PREVIEW_W, PREVIEW_H);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, 0);
    lv_image_set_src(s_img, &s_dsc[0]);

    s_status = lv_label_create(root);
    lv_obj_set_style_bg_color(s_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_status, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_status, 12, 0);
    lv_obj_set_style_pad_hor(s_status, 12, 0);
    lv_obj_set_style_pad_ver(s_status, 4, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_text(s_status, "");
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);

    /* Shutter: white disc in a ring, in the bar under the preview */
    lv_obj_t *ring = lv_obj_create(root);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 84, 84);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 4, 0);
    lv_obj_set_style_border_color(ring, lv_color_white(), 0);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, PREVIEW_H + 10);
    lv_obj_remove_flag(ring, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    s_shutter = lv_button_create(ring);
    lv_obj_remove_style_all(s_shutter);
    lv_obj_set_size(s_shutter, 64, 64);
    lv_obj_center(s_shutter);
    lv_obj_set_ext_click_area(s_shutter, 14);
    lv_obj_set_style_radius(s_shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_shutter, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_shutter, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_shutter, lv_color_hex(0xB0B8C0), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_shutter, lv_color_hex(0x5A6470), LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_shutter, on_shutter, LV_EVENT_CLICKED, nullptr);
}

} // namespace

namespace esp_brookesia::apps {

PhoneCamera *PhoneCamera::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneCamera();
    }
    return _instance;
}

PhoneCamera::PhoneCamera():
    App(phone_tr("Máy ảnh", "Camera"), &phone_camera_icon, true, true, true)
{
}

PhoneCamera::~PhoneCamera()
{
}

bool PhoneCamera::run()
{
    if (!alloc_preview()) {
        ESP_LOGE(TAG, "no memory for the preview");
        free_preview();
        return false;
    }
    build_ui();
    lv_timer_create(on_timer, 100, nullptr);
    start_worker();
    return true;
}

bool PhoneCamera::back()
{
    return notifyCoreClosed();
}

bool PhoneCamera::close()
{
    stop_worker();
    /* the image still points at the preview buffers until the core deletes the screen */
    if (s_img) {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_img, nullptr);
    }
    free_preview();
    return true;
}

/* In the background (home, recents): the sensor is powered down */
bool PhoneCamera::pause()
{
    stop_worker();
    return true;
}

bool PhoneCamera::resume()
{
    start_worker();
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneCamera, APP_NAME, []()
{
    return std::shared_ptr<PhoneCamera>(PhoneCamera::requestInstance(), [](PhoneCamera * p) {});
})

} // namespace esp_brookesia::apps
