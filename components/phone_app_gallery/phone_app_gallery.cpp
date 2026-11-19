/*
 * Gallery app: the JPEG photos of /sdcard/DCIM as a grid of thumbnails, newest first. A tap opens
 * a photo in the viewer (previous / next by buttons or swipe, delete).
 *
 * Photos are decoded by the hardware JPEG decoder into RGB565, then cropped and scaled by the PPA.
 * Thumbnails are made one by one in a worker task, so the grid stays responsive; the viewer
 * decodes in the LVGL task. Both share the decoder and the PPA under one mutex.
 */
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "GALLERY"
#include "esp_lib_utils.h"
#include "phone_ui.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_storage.h"
#include "phone_app_gallery.hpp"

#define APP_NAME "Thư viện ảnh"

extern "C" const lv_image_dsc_t phone_gallery_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_gallery";

constexpr const char *PHOTO_DIR = "/sdcard/DCIM";
constexpr int MAX_PHOTOS = 120;             // newest ones; each thumbnail takes 45 KB of PSRAM
constexpr int THUMB = 150;                  // thumbnail and grid cell size, px
constexpr int GRID_GAP = 10;
constexpr int HEADER_H = 56;
constexpr int ALIGN = 128;                  // L2 cache line, required for PPA output buffers
constexpr size_t MAX_FILE = 4 * 1024 * 1024;
constexpr int MAX_SIDE = 2048;              // larger pictures are not decoded

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_CELL = 0x1E252C;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_DANGER = 0xE5484D;

struct Item {
    std::string name;
    lv_image_dsc_t dsc;         // thumbnail, valid once thumb is set
    uint8_t *thumb;
    lv_obj_t *cell;
};

struct Picture {                // one decoded JPEG
    uint8_t *data;
    int w, h;                   // picture size
    int buf_w, buf_h;           // size of the decoded buffer (rounded up to 16)
};

const char *s_hold_name;    // holder of the card while the app runs (phone_storage_hold)
lv_obj_t *s_root;
lv_obj_t *s_grid;
lv_obj_t *s_count;
lv_obj_t *s_empty;
lv_obj_t *s_viewer;
lv_obj_t *s_view_img;
lv_obj_t *s_view_title;
lv_obj_t *s_view_delete;
lv_image_dsc_t s_view_dsc;
uint8_t *s_view_buf;
size_t s_view_buf_size;
int s_view_w, s_view_h;         // area available to the viewer picture
int s_view_index = -1;
uint32_t s_delete_armed_until;

std::vector<Item> s_items;
jpeg_decoder_handle_t s_dec;
ppa_client_handle_t s_ppa;
SemaphoreHandle_t s_codec;      // decoder + PPA
SemaphoreHandle_t s_done;
TaskHandle_t s_task;
std::atomic<bool> s_stop;

size_t align_up(size_t n)
{
    return (n + ALIGN - 1) & ~static_cast<size_t>(ALIGN - 1);
}

bool is_jpeg(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0);
}

/* Decode a file to RGB565 with the hardware decoder. The caller frees pic.data. */
bool decode_file(const std::string &name, Picture &pic)
{
    pic = {};
    const std::string path = std::string(PHOTO_DIR) + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) > MAX_FILE) {
        return false;
    }
    const jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    const jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t in_size = 0, out_size = 0;
    auto *in = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(st.st_size, &in_cfg, &in_size));
    bool ok = false;
    if (in) {
        FILE *file = fopen(path.c_str(), "rb");
        ok = file && fread(in, 1, st.st_size, file) == static_cast<size_t>(st.st_size);
        if (file) {
            fclose(file);
        }
    }
    jpeg_decode_picture_info_t info = {};
    ok = ok && jpeg_decoder_get_info(in, st.st_size, &info) == ESP_OK;
    ok = ok && info.width > 0 && info.height > 0 && info.width <= MAX_SIDE && info.height <= MAX_SIDE;
    if (ok) {
        pic.w = info.width;
        pic.h = info.height;
        pic.buf_w = (pic.w + 15) & ~15;
        pic.buf_h = (pic.h + 15) & ~15;
        pic.data = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(static_cast<size_t>(pic.buf_w) * pic.buf_h * 2,
                                          &out_cfg, &out_size));
        const jpeg_decode_cfg_t cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t len = 0;
        ok = pic.data && jpeg_decoder_process(s_dec, &cfg, in, st.st_size, pic.data, out_size, &len) == ESP_OK;
    }
    free(in);
    if (!ok) {
        free(pic.data);
        pic.data = nullptr;
        ESP_LOGW(TAG, "cannot decode %s", name.c_str());
    }
    return ok;
}

/* Scale the centered block_w x block_h block of a picture by steps/16 into out */
bool scale_block(const Picture &pic, int block_w, int block_h, int steps, uint8_t *out, size_t out_size,
                 int out_w, int out_h)
{
    ppa_srm_oper_config_t op = {};
    op.in.buffer = pic.data;
    op.in.pic_w = pic.buf_w;
    op.in.pic_h = pic.buf_h;
    op.in.block_w = block_w;
    op.in.block_h = block_h;
    op.in.block_offset_x = (pic.w - block_w) / 2;
    op.in.block_offset_y = (pic.h - block_h) / 2;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = out;
    op.out.buffer_size = out_size;
    op.out.pic_w = out_w;
    op.out.pic_h = out_h;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = steps / 16.0f;
    op.scale_y = steps / 16.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK;
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

/* Square thumbnail of the center of the picture; the PPA scales in 1/16 steps */
uint8_t *make_thumb(const std::string &name, int &side_out)
{
    Picture pic;
    uint8_t *thumb = nullptr;
    xSemaphoreTake(s_codec, portMAX_DELAY);
    if (decode_file(name, pic)) {
        const int side = std::min(pic.w, pic.h);
        int steps = (THUMB * 16 + side - 1) / side;         // round the scale up, then crop a bit more
        steps = std::clamp(steps, 1, 32);
        const int block = std::min(side, THUMB * 16 / steps) & ~1;
        const int out = block * steps / 16;
        const size_t size = align_up(static_cast<size_t>(out) * out * 2);
        thumb = static_cast<uint8_t *>(heap_caps_aligned_calloc(ALIGN, 1, size, MALLOC_CAP_SPIRAM));
        if (thumb && scale_block(pic, block, block, steps, thumb, size, out, out)) {
            side_out = out;
        } else {
            heap_caps_free(thumb);
            thumb = nullptr;
        }
        free(pic.data);
    }
    xSemaphoreGive(s_codec);
    return thumb;
}

void worker(void *)
{
    for (size_t i = 0; i < s_items.size() && !s_stop; i++) {
        int side = 0;
        uint8_t *thumb = make_thumb(s_items[i].name, side);
        bool shown = false;
        while (thumb && !shown && !s_stop) {
            if (bsp_display_lock(50)) {
                Item &it = s_items[i];
                it.thumb = thumb;
                fill_dsc(it.dsc, thumb, side, side);
                if (it.cell) {
                    lv_obj_t *img = lv_image_create(it.cell);
                    lv_image_set_src(img, &it.dsc);
                    lv_obj_center(img);
                    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
                }
                shown = true;
                bsp_display_unlock();
            }
        }
        if (thumb && !shown) {
            heap_caps_free(thumb);
        }
    }
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

/* Called with the LVGL lock held: the worker never waits for that lock longer than 50 ms */
void stop_worker()
{
    if (!s_task) {
        return;
    }
    s_stop = true;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "thumbnail task did not stop");
    }
    s_task = nullptr;
}

void start_worker()
{
    s_stop = false;
    if (s_items.empty()) {
        return;
    }
    if (xTaskCreate(worker, "phone_gallery", 6144, nullptr, 3, &s_task) != pdPASS) {
        s_task = nullptr;
    }
}

void free_items()
{
    for (Item &it : s_items) {
        heap_caps_free(it.thumb);
    }
    s_items.clear();
}

void scan_photos()
{
    std::vector<std::string> names;
    DIR *dir = opendir(PHOTO_DIR);
    if (dir) {
        while (struct dirent *e = readdir(dir)) {
            if (e->d_type != DT_DIR && is_jpeg(e->d_name)) {
                names.emplace_back(e->d_name);
            }
        }
        closedir(dir);
    }
    /* IMG_<date>_<time>.jpg: the name order is the time order; newest first */
    std::sort(names.begin(), names.end(), std::greater<std::string>());
    if (names.size() > MAX_PHOTOS) {
        names.resize(MAX_PHOTOS);
    }
    for (std::string &n : names) {
        Item it = {};
        it.name = std::move(n);
        s_items.push_back(std::move(it));
    }
}

void show_viewer(int index);

void on_cell(lv_event_t *e)
{
    show_viewer(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

/* Rebuild the grid from the card; stops and restarts the thumbnail task */
void reload()
{
    stop_worker();
    lv_obj_clean(s_grid);
    free_items();
    const bool mounted = phone_storage_get(nullptr, nullptr) == PHONE_STORAGE_MOUNTED;
    if (mounted) {
        scan_photos();
    }
    for (size_t i = 0; i < s_items.size(); i++) {
        lv_obj_t *cell = lv_obj_create(s_grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, THUMB, THUMB);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(COLOR_CELL), 0);
        lv_obj_set_style_radius(cell, 8, 0);
        lv_obj_set_style_clip_corner(cell, true, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, on_cell, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        s_items[i].cell = cell;
    }
    lv_label_set_text_fmt(s_count, "%d %s", static_cast<int>(s_items.size()), T("ảnh", "photos"));
    if (s_items.empty()) {
        lv_label_set_text(s_empty, mounted ? T("Chưa có ảnh nào.\nHãy chụp bằng app Máy ảnh.",
                                               "No photos yet.\nTake some with the Camera app.")
                          : T("Không có thẻ nhớ", "No microSD card"));
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    start_worker();
}

void close_viewer()
{
    lv_obj_add_flag(s_viewer, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_view_img, nullptr);
    s_view_index = -1;
}

void disarm_delete()
{
    s_delete_armed_until = 0;
    lv_obj_set_style_bg_color(s_view_delete, lv_color_hex(0x2A333C), 0);
    lv_label_set_text(lv_obj_get_child(s_view_delete, 0), LV_SYMBOL_TRASH);
}

void show_viewer(int index)
{
    if (index < 0 || index >= static_cast<int>(s_items.size())) {
        return;
    }
    s_view_index = index;
    disarm_delete();
    lv_label_set_text_fmt(s_view_title, "%d / %d   %s", index + 1, static_cast<int>(s_items.size()),
                          s_items[index].name.c_str());
    lv_obj_remove_flag(s_viewer, LV_OBJ_FLAG_HIDDEN);

    /* Whole picture, scaled down to fit (1/16 steps, never enlarged) */
    Picture pic;
    bool ok = false;
    xSemaphoreTake(s_codec, portMAX_DELAY);
    if (decode_file(s_items[index].name, pic)) {
        int steps = std::min(s_view_w * 16 / pic.w, s_view_h * 16 / pic.h);
        steps = std::clamp(steps, 1, 16);
        const int w = pic.w * steps / 16;
        const int h = pic.h * steps / 16;
        ok = w <= s_view_w && h <= s_view_h && scale_block(pic, pic.w, pic.h, steps, s_view_buf, s_view_buf_size, w, h);
        if (ok) {
            fill_dsc(s_view_dsc, s_view_buf, w, h);
        }
        free(pic.data);
    }
    xSemaphoreGive(s_codec);
    if (ok) {
        lv_image_cache_drop(&s_view_dsc);       /* same descriptor, new pixels */
        lv_image_set_src(s_view_img, &s_view_dsc);
        lv_obj_invalidate(s_view_img);
    } else {
        lv_image_set_src(s_view_img, nullptr);
        lv_label_set_text_fmt(s_view_title, "%s   %s", T("Không mở được", "Cannot open"), s_items[index].name.c_str());
    }
}

void step_viewer(int delta)
{
    const int next = s_view_index + delta;
    if (next >= 0 && next < static_cast<int>(s_items.size())) {
        show_viewer(next);
    }
}

void on_prev(lv_event_t *)
{
    step_viewer(-1);
}

void on_next(lv_event_t *)
{
    step_viewer(1);
}

void on_swipe(lv_event_t *)
{
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) {
        step_viewer(1);
    } else if (dir == LV_DIR_RIGHT) {
        step_viewer(-1);
    }
}

/* First tap arms the button for 3 s, the second one deletes */
void on_delete(lv_event_t *)
{
    if (s_view_index < 0) {
        return;
    }
    if (lv_tick_get() >= s_delete_armed_until) {
        s_delete_armed_until = lv_tick_get() + 3000;
        lv_obj_set_style_bg_color(s_view_delete, lv_color_hex(COLOR_DANGER), 0);
        lv_label_set_text(lv_obj_get_child(s_view_delete, 0), T("Xóa?", "Delete?"));
        return;
    }
    const int index = s_view_index;
    const std::string path = std::string(PHOTO_DIR) + "/" + s_items[index].name;
    if (unlink(path.c_str()) != 0) {
        ESP_LOGE(TAG, "cannot delete %s", path.c_str());
    }
    close_viewer();
    reload();
    if (!s_items.empty()) {
        show_viewer(std::min(index, static_cast<int>(s_items.size()) - 1));
    }
}

void on_timer(lv_timer_t *)
{
    if (s_root && s_delete_armed_until && lv_tick_get() >= s_delete_armed_until) {
        disarm_delete();
    }
}

lv_obj_t *make_round_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_min_width(b, 48, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A333C), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x4A5560), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
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
    lv_obj_update_layout(root);
    const int root_w = lv_obj_get_width(root);
    const int root_h = lv_obj_get_height(root);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, T("Thư viện ảnh", "Gallery"));
    lv_obj_set_style_text_font(title, phone_font_by_size(24), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 14);
    s_count = lv_label_create(root);
    lv_obj_set_style_text_color(s_count, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_count, phone_font_by_size(18), 0);
    lv_obj_align(s_count, LV_ALIGN_TOP_RIGHT, -16, 18);
    lv_label_set_text(s_count, "");

    s_grid = lv_obj_create(root);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, root_w, root_h - HEADER_H);
    lv_obj_set_y(s_grid, HEADER_H);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(s_grid, (root_w - 3 * THUMB - 2 * GRID_GAP) / 2, 0);
    lv_obj_set_style_pad_bottom(s_grid, 28, 0);
    lv_obj_set_style_pad_column(s_grid, GRID_GAP, 0);
    lv_obj_set_style_pad_row(s_grid, GRID_GAP, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    phone_ui_scrollbar(s_grid);

    s_empty = lv_label_create(root);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_empty);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

    /* Viewer: covers the grid */
    s_viewer = lv_obj_create(root);
    lv_obj_remove_style_all(s_viewer);
    lv_obj_set_size(s_viewer, root_w, root_h);
    lv_obj_set_style_bg_opa(s_viewer, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_viewer, lv_color_black(), 0);
    lv_obj_remove_flag(s_viewer, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_flag(s_viewer, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_event_cb(s_viewer, on_swipe, LV_EVENT_GESTURE, nullptr);

    s_view_w = root_w;
    s_view_h = root_h;
    s_view_img = lv_image_create(s_viewer);
    lv_obj_center(s_view_img);
    lv_obj_remove_flag(s_view_img, LV_OBJ_FLAG_CLICKABLE);

    s_view_title = lv_label_create(s_viewer);
    lv_obj_set_style_text_font(s_view_title, phone_font_by_size(16), 0);
    lv_obj_set_style_bg_color(s_view_title, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_view_title, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_view_title, 10, 0);
    lv_obj_set_style_pad_hor(s_view_title, 10, 0);
    lv_obj_set_style_pad_ver(s_view_title, 4, 0);
    lv_obj_align(s_view_title, LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(s_view_title, "");

    lv_obj_align(make_round_button(s_viewer, LV_SYMBOL_LEFT, on_prev), LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_align(make_round_button(s_viewer, LV_SYMBOL_RIGHT, on_next), LV_ALIGN_RIGHT_MID, -10, 0);
    s_view_delete = make_round_button(s_viewer, LV_SYMBOL_TRASH, on_delete);
    lv_obj_align(s_view_delete, LV_ALIGN_BOTTOM_MID, 0, -40);

    /* The strip where the swipe-up for the navigation bar starts: a touch there must not scroll
     * the list under it. The system reads that gesture straight from the touch panel. */
    lv_obj_t *guard = lv_obj_create(root);
    lv_obj_remove_style_all(guard);
    lv_obj_set_size(guard, LV_PCT(100), 36);
    lv_obj_align(guard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(guard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(guard, LV_OBJ_FLAG_SCROLLABLE);
}

bool init_codec()
{
    if (!s_codec) {
        s_codec = xSemaphoreCreateMutex();
    }
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
    }
    const jpeg_decode_engine_cfg_t dec_cfg = {.intr_priority = 0, .timeout_ms = 2000};
    const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};
    if (!s_dec && jpeg_new_decoder_engine(&dec_cfg, &s_dec) != ESP_OK) {
        s_dec = nullptr;
        return false;
    }
    if (!s_ppa && ppa_register_client(&ppa_cfg, &s_ppa) != ESP_OK) {
        s_ppa = nullptr;
        return false;
    }
    return true;
}

void deinit_codec()
{
    if (s_dec) {
        jpeg_del_decoder_engine(s_dec);
        s_dec = nullptr;
    }
    if (s_ppa) {
        ppa_unregister_client(s_ppa);
        s_ppa = nullptr;
    }
}

} // namespace

namespace esp_brookesia::apps {

PhoneGallery *PhoneGallery::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneGallery();
    }
    return _instance;
}

PhoneGallery::PhoneGallery():
    App(phone_tr("Thư viện ảnh", "Gallery"), &phone_gallery_icon, true, true, true)
{
}

PhoneGallery::~PhoneGallery()
{
}

bool PhoneGallery::run()
{
    s_hold_name = phone_tr("Thư viện ảnh", "Gallery");
    phone_storage_hold(s_hold_name);      /* the thumbnail worker reads in the background */
    if (!init_codec()) {
        ESP_LOGE(TAG, "no JPEG decoder or PPA");
        deinit_codec();
        return false;
    }
    build_ui();
    s_view_buf_size = align_up(static_cast<size_t>(s_view_w) * s_view_h * 2);
    s_view_buf = static_cast<uint8_t *>(heap_caps_aligned_calloc(ALIGN, 1, s_view_buf_size, MALLOC_CAP_SPIRAM));
    if (!s_view_buf) {
        deinit_codec();
        return false;
    }
    lv_timer_create(on_timer, 250, nullptr);
    reload();
    return true;
}

bool PhoneGallery::back()
{
    if (s_view_index >= 0) {
        close_viewer();
        return true;
    }
    return notifyCoreClosed();
}

bool PhoneGallery::close()
{
    stop_worker();
    phone_storage_release(s_hold_name);
    /* the widgets still point at the buffers until the core deletes the screen */
    if (s_root) {
        lv_obj_clean(s_root);
    }
    s_view_index = -1;
    free_items();
    heap_caps_free(s_view_buf);
    s_view_buf = nullptr;
    deinit_codec();
    return true;
}

/* Back from another app (the camera): show the new photos */
bool PhoneGallery::resume()
{
    if (s_root && s_view_index < 0) {
        reload();
    }
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneGallery, APP_NAME, []()
{
    return std::shared_ptr<PhoneGallery>(PhoneGallery::requestInstance(), [](PhoneGallery * p) {});
})

} // namespace esp_brookesia::apps
