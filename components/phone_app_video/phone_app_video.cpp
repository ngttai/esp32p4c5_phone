/*
 * Video app: plays Motion JPEG .avi files from the microSD card, with 16-bit PCM sound.
 *
 * The ESP32-P4 has no H.264 decoder, so the video must be MJPEG: every frame is a JPEG picture,
 * decoded by the hardware JPEG decoder, then scaled (and turned, for landscape videos) by the PPA
 * into one of two buffers shown by an LVGL image. A player task walks the "movi" list of the AVI
 * file in order: video chunks are shown at the frame rate of the file (late frames are dropped),
 * audio chunks go to the speaker codec.
 *
 * Convert a video with ffmpeg:
 *   ffmpeg -i in.mp4 -vf scale=800:480 -r 20 -c:v mjpeg -q:v 5 -c:a pcm_s16le -ar 22050 -ac 1 out.avi
 */
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <vector>
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "VIDEO"
#include "esp_lib_utils.h"
#include "phone_audio.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"
#include "phone_storage.h"
#include "phone_app_video.hpp"

#define APP_NAME "Video"

extern "C" const lv_image_dsc_t phone_video_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_video";

constexpr const char *CARD = "/sdcard";
constexpr int MAX_FILES = 200;
constexpr int SCAN_DEPTH = 2;
constexpr int ALIGN = 128;                  // L2 cache line, required for PPA output buffers
constexpr int MAX_SIDE = 1920;
constexpr size_t FRAME_MAX = 2 * 1024 * 1024;
constexpr size_t AUDIO_MAX = 256 * 1024;
constexpr int HEADER_H = 60;
constexpr uint32_t OVERLAY_MS = 3000;       // the controls hide after this long
/* Landscape videos are turned to fill the portrait screen (counter-clockwise) */
constexpr ppa_srm_rotation_angle_t LANDSCAPE_ROTATION = PPA_SRM_ROTATION_ANGLE_90;

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_ROW = 0x1E252C;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0x8E6BFF;

/* ---- AVI ---- */

struct Avi {
    FILE *file;
    long movi_start, movi_end;
    uint32_t usec_per_frame;
    uint32_t total_frames;
    int width, height;
    int video_stream, audio_stream;     // stream numbers, -1 when absent
    uint16_t audio_format, audio_channels, audio_bits;
    uint32_t audio_rate;
};

uint32_t le32(const uint8_t *p)
{
    return p[0] | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}

/* Read the headers up to the "movi" list */
bool avi_open(const char *path, Avi &avi)
{
    avi = {};
    avi.video_stream = avi.audio_stream = -1;
    avi.file = fopen(path, "rb");
    uint8_t h[12];
    if (!avi.file || fread(h, 1, 12, avi.file) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "AVI ", 4) != 0) {
        return false;
    }
    setvbuf(avi.file, nullptr, _IOFBF, 32 * 1024);
    int stream = -1;
    bool stream_is_video = false, stream_is_audio = false;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, avi.file) != 8) {
            return false;
        }
        const uint32_t size = le32(c + 4);
        if (memcmp(c, "LIST", 4) == 0) {
            uint8_t type[4];
            if (fread(type, 1, 4, avi.file) != 4) {
                return false;
            }
            if (memcmp(type, "movi", 4) == 0) {
                avi.movi_start = ftell(avi.file);
                avi.movi_end = avi.movi_start + static_cast<long>(size) - 4;
                break;
            }
            if (memcmp(type, "strl", 4) == 0) {
                stream++;
                stream_is_video = stream_is_audio = false;
            } else if (memcmp(type, "hdrl", 4) != 0) {
                fseek(avi.file, static_cast<long>((size - 4 + 1) & ~1u), SEEK_CUR);
            }
            continue;       /* hdrl and strl: their chunks follow */
        }
        uint8_t d[64] = {};
        const size_t n = size < sizeof(d) ? size : sizeof(d);
        if (fread(d, 1, n, avi.file) != n) {
            return false;
        }
        fseek(avi.file, static_cast<long>(((size + 1) & ~1u) - n), SEEK_CUR);
        if (memcmp(c, "avih", 4) == 0 && n >= 40) {
            avi.usec_per_frame = le32(d);
            avi.total_frames = le32(d + 16);
            avi.width = le32(d + 32);
            avi.height = le32(d + 36);
        } else if (memcmp(c, "strh", 4) == 0 && n >= 8) {
            stream_is_video = memcmp(d, "vids", 4) == 0;
            stream_is_audio = memcmp(d, "auds", 4) == 0;
            if (stream_is_video && avi.video_stream < 0) {
                avi.video_stream = stream;
            }
        } else if (memcmp(c, "strf", 4) == 0 && stream_is_audio && avi.audio_stream < 0 && n >= 16) {
            avi.audio_stream = stream;
            avi.audio_format = le16(d);
            avi.audio_channels = le16(d + 2);
            avi.audio_rate = le32(d + 4);
            avi.audio_bits = le16(d + 14);
        }
    }
    if (avi.usec_per_frame == 0) {
        avi.usec_per_frame = 50000;
    }
    return avi.video_stream >= 0 && avi.movi_end > avi.movi_start;
}

/* ---- player ---- */

enum State { ST_IDLE, ST_PLAYING, ST_ENDED, ST_ERROR };

lv_obj_t *s_root;
lv_obj_t *s_list;
lv_obj_t *s_player;         // covers the list while a video is open
lv_obj_t *s_img;
lv_obj_t *s_overlay;
lv_obj_t *s_title;
lv_obj_t *s_time;
lv_obj_t *s_bar;
lv_obj_t *s_play_label;
lv_obj_t *s_message;
int s_area_w, s_area_h;     // room for the picture
uint8_t *s_out[2];
size_t s_out_size;
lv_image_dsc_t s_dsc[2];
uint32_t s_overlay_until;
int s_shown_state = -1;
bool s_shown_paused;

std::vector<std::string> s_files;
std::string s_path;
TaskHandle_t s_task;
SemaphoreHandle_t s_done;
std::atomic<bool> s_stop;
std::atomic<bool> s_paused;
std::atomic<int> s_state{ST_IDLE};
std::atomic<uint32_t> s_frame;
std::atomic<uint32_t> s_total;
std::atomic<uint32_t> s_usec_per_frame{50000};

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

void player_task(void *)
{
    Avi avi = {};
    jpeg_decoder_handle_t dec = nullptr;
    ppa_client_handle_t ppa = nullptr;
    uint8_t *in = nullptr, *rgb = nullptr, *audio = nullptr;
    size_t in_size = 0, rgb_size = 0, audio_size = 0;
    esp_codec_dev_handle_t spk = nullptr;
    bool spk_open = false;
    int volume = -1;
    int buf_w = 0, buf_h = 0, out_w = 0, out_h = 0, steps = 16;
    bool turned = false;
    int back = 0, bad_frames = 0;
    uint32_t frame = 0;
    int64_t start_us = 0;
    bool ok = false;
    const jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    const jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    const jpeg_decode_engine_cfg_t eng_cfg = {.intr_priority = 0, .timeout_ms = 1000};
    const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};
    const jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    if (!avi_open(s_path.c_str(), avi) || jpeg_new_decoder_engine(&eng_cfg, &dec) != ESP_OK ||
            ppa_register_client(&ppa_cfg, &ppa) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open %s", s_path.c_str());
        goto done;
    }
    s_total = avi.total_frames;
    s_usec_per_frame = avi.usec_per_frame;
    ESP_LOGI(TAG, "%s: %d x %d, %u frames, %u us/frame, audio fmt %u, %u Hz, %u ch, %u bit", s_path.c_str(), avi.width,
             avi.height, static_cast<unsigned>(avi.total_frames), static_cast<unsigned>(avi.usec_per_frame),
             avi.audio_format, static_cast<unsigned>(avi.audio_rate), avi.audio_channels, avi.audio_bits);

    /* sound: 16-bit PCM only; the speaker may be busy (music) */
    if (avi.audio_stream >= 0 && avi.audio_format == 1 && avi.audio_bits == 16 && avi.audio_rate > 0 &&
            (avi.audio_channels == 1 || avi.audio_channels == 2)) {
        spk = static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take());
        if (spk) {
            esp_codec_dev_sample_info_t fs = {};
            fs.sample_rate = avi.audio_rate;
            fs.channel = 1;
            fs.bits_per_sample = 16;
            spk_open = esp_codec_dev_open(spk, &fs) == ESP_CODEC_DEV_OK;
        }
    }

    ok = true;
    s_state = ST_PLAYING;
    start_us = esp_timer_get_time();
    fseek(avi.file, avi.movi_start, SEEK_SET);
    while (!s_stop) {
        if (s_paused || phone_lock_is_locked()) {
            /* hold the clock while paused or while the lock screen is up */
            const int64_t t0 = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(40));
            start_us += esp_timer_get_time() - t0;
            continue;
        }
        uint8_t c[8];
        if (ftell(avi.file) + 8 > avi.movi_end || fread(c, 1, 8, avi.file) != 8) {
            break;
        }
        if (memcmp(c, "LIST", 4) == 0) {
            fseek(avi.file, 4, SEEK_CUR);       /* "rec " group: its chunks follow */
            continue;
        }
        const uint32_t size = le32(c + 4);
        const long next = ftell(avi.file) + static_cast<long>((size + 1) & ~1u);
        const int stream = (c[0] - '0') * 10 + (c[1] - '0');
        const bool is_video = stream == avi.video_stream && c[2] == 'd' && (c[3] == 'c' || c[3] == 'b');
        const bool is_audio = stream == avi.audio_stream && c[2] == 'w' && c[3] == 'b';

        if (is_video) {
            const int64_t due = start_us + static_cast<int64_t>(frame) * avi.usec_per_frame;
            const int64_t now = esp_timer_get_time();
            frame++;
            s_frame = frame;
            /* more than two frames late: drop this one */
            const bool drop = now - due > 2 * static_cast<int64_t>(avi.usec_per_frame);
            if (size > 0 && size <= FRAME_MAX && !drop) {
                if (size > in_size) {
                    free(in);
                    in = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(size + size / 2, &in_cfg, &in_size));
                    if (!in) {
                        in_size = 0;
                    }
                }
                if (in && fread(in, 1, size, avi.file) == size) {
                    if (!rgb) {
                        jpeg_decode_picture_info_t info = {};
                        if (jpeg_decoder_get_info(in, size, &info) == ESP_OK && info.width > 0 && info.height > 0 &&
                                info.width <= MAX_SIDE && info.height <= MAX_SIDE) {
                            avi.width = info.width;
                            avi.height = info.height;
                            buf_w = (avi.width + 15) & ~15;
                            buf_h = (avi.height + 15) & ~15;
                            rgb = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(static_cast<size_t>(buf_w) * buf_h * 2,
                                                         &out_cfg, &rgb_size));
                            /* fit the screen in 1/16 steps; landscape is turned */
                            turned = avi.width > avi.height;
                            const int rw = turned ? avi.height : avi.width;
                            const int rh = turned ? avi.width : avi.height;
                            steps = std::clamp(std::min(s_area_w * 16 / rw, s_area_h * 16 / rh), 1, 48);
                            out_w = rw * steps / 16;
                            out_h = rh * steps / 16;
                        }
                    }
                    uint32_t len = 0;
                    if (rgb && jpeg_decoder_process(dec, &dec_cfg, in, size, rgb, rgb_size, &len) == ESP_OK) {
                        ppa_srm_oper_config_t op = {};
                        op.in.buffer = rgb;
                        op.in.pic_w = buf_w;
                        op.in.pic_h = buf_h;
                        op.in.block_w = avi.width;
                        op.in.block_h = avi.height;
                        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
                        op.out.buffer = s_out[back];
                        op.out.buffer_size = s_out_size;
                        op.out.pic_w = out_w;
                        op.out.pic_h = out_h;
                        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
                        op.rotation_angle = turned ? LANDSCAPE_ROTATION : PPA_SRM_ROTATION_ANGLE_0;
                        op.scale_x = steps / 16.0f;
                        op.scale_y = steps / 16.0f;
                        op.mode = PPA_TRANS_MODE_BLOCKING;
                        if (ppa_do_scale_rotate_mirror(ppa, &op) == ESP_OK) {
                            /* wait for the time of this frame, then show it */
                            const int64_t wait = due - esp_timer_get_time();
                            if (wait > 1000) {
                                vTaskDelay(pdMS_TO_TICKS(wait / 1000));
                            }
                            if (bsp_display_lock(50)) {
                                if (s_img && !s_stop) {
                                    fill_dsc(s_dsc[back], s_out[back], out_w, out_h);
                                    lv_image_set_src(s_img, &s_dsc[back]);
                                    back ^= 1;
                                }
                                bsp_display_unlock();
                            }
                        }
                        bad_frames = 0;
                    } else if (++bad_frames > 10) {
                        ESP_LOGE(TAG, "frames cannot be decoded: not baseline MJPEG?");
                        ok = false;
                        break;
                    }
                }
            }
        } else if (is_audio && spk_open && size > 0 && size <= AUDIO_MAX) {
            if (size > audio_size) {
                free(audio);
                audio = static_cast<uint8_t *>(malloc(size));
                audio_size = audio ? size : 0;
            }
            if (audio && fread(audio, 1, size, avi.file) == size) {
                auto *s = reinterpret_cast<int16_t *>(audio);
                size_t frames = size / (2 * avi.audio_channels);
                if (avi.audio_channels == 2) {
                    for (size_t i = 0; i < frames; i++) {
                        s[i] = static_cast<int16_t>((s[2 * i] + s[2 * i + 1]) / 2);
                    }
                }
                const int v = phone_audio_get_volume();
                if (v != volume) {
                    volume = v;
                    esp_codec_dev_set_out_vol(spk, v);
                }
                esp_codec_dev_write(spk, s, frames * 2);
            }
        }
        fseek(avi.file, next, SEEK_SET);
    }

done:
    if (spk_open) {
        esp_codec_dev_close(spk);
    }
    if (spk) {
        phone_audio_speaker_give();
    }
    if (ppa) {
        ppa_unregister_client(ppa);
    }
    if (dec) {
        jpeg_del_decoder_engine(dec);
    }
    if (avi.file) {
        fclose(avi.file);
    }
    free(in);
    free(rgb);
    free(audio);
    s_state = ok ? ST_ENDED : ST_ERROR;
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

/* Called with the LVGL lock held: the player never waits for that lock longer than 50 ms */
void stop_player()
{
    if (!s_task) {
        return;
    }
    s_stop = true;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "player task did not stop");
    }
    s_task = nullptr;
    s_state = ST_IDLE;
}

/* ---- UI ---- */

std::string name_of(const std::string &path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void show_overlay()
{
    s_overlay_until = lv_tick_get() + OVERLAY_MS;
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void close_player()
{
    stop_player();
    lv_image_set_src(s_img, nullptr);
    lv_obj_add_flag(s_player, LV_OBJ_FLAG_HIDDEN);
}

void open_video(int index)
{
    if (index < 0 || index >= static_cast<int>(s_files.size()) || s_task) {
        return;
    }
    s_path = s_files[index];
    s_stop = false;
    s_paused = false;
    s_frame = 0;
    s_total = 0;
    s_shown_state = -1;
    lv_label_set_text(s_title, name_of(s_path).c_str());
    lv_label_set_text(s_message, "");
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_image_set_src(s_img, nullptr);
    lv_obj_remove_flag(s_player, LV_OBJ_FLAG_HIDDEN);
    show_overlay();
    s_state = ST_IDLE;
    if (xTaskCreate(player_task, "phone_video", 8192, nullptr, 4, &s_task) != pdPASS) {
        s_task = nullptr;
        s_state = ST_ERROR;
    }
}

void on_row(lv_event_t *e)
{
    open_video(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void on_play(lv_event_t *)
{
    if (s_state == ST_PLAYING) {
        s_paused = !s_paused;
    }
    show_overlay();
}

/* A tap on the picture shows or hides the controls */
void on_tap(lv_event_t *)
{
    if (lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        show_overlay();
    } else {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void on_timer(lv_timer_t *)
{
    if (!s_root || lv_obj_has_flag(s_player, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    const int state = s_state;
    const bool paused = s_paused;
    if (state != s_shown_state || paused != s_shown_paused) {
        s_shown_state = state;
        s_shown_paused = paused;
        lv_label_set_text(s_play_label, state == ST_PLAYING && !paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        if (state == ST_ERROR) {
            lv_label_set_text(s_message, T("Không phát được video này.\nCần tệp .avi dạng MJPEG.",
                                           "Cannot play this video.\nIt must be an MJPEG .avi file."));
            show_overlay();
        } else if (state == ST_ENDED) {
            close_player();
            return;
        }
    }
    const uint32_t total = s_total;
    const uint32_t frame = s_frame;
    const uint32_t upf = s_usec_per_frame;
    const uint32_t sec = static_cast<uint32_t>(static_cast<uint64_t>(frame) * upf / 1000000);
    const uint32_t all = static_cast<uint32_t>(static_cast<uint64_t>(total) * upf / 1000000);
    lv_label_set_text_fmt(s_time, "%u:%02u / %u:%02u", static_cast<unsigned>(sec / 60), static_cast<unsigned>(sec % 60),
                          static_cast<unsigned>(all / 60), static_cast<unsigned>(all % 60));
    lv_bar_set_value(s_bar, total ? static_cast<int>(static_cast<uint64_t>(frame) * 1000 / total) : 0, LV_ANIM_OFF);

    if (state == ST_PLAYING && !paused && !phone_lock_is_locked() && lv_obj_get_screen(s_root) == lv_screen_active()) {
        lv_display_trigger_activity(nullptr);       /* being watched: keep the screen on */
        if (lv_tick_get() >= s_overlay_until) {
            lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void add_dir(const std::string &dir, int depth)
{
    DIR *d = opendir(dir.c_str());
    if (!d) {
        return;
    }
    std::vector<std::string> subdirs;
    while (struct dirent *e = readdir(d)) {
        if (static_cast<int>(s_files.size()) >= MAX_FILES || e->d_name[0] == '.') {
            continue;
        }
        const std::string path = dir + "/" + e->d_name;
        const char *dot = strrchr(e->d_name, '.');
        if (e->d_type == DT_DIR) {
            if (depth > 0) {
                subdirs.push_back(path);
            }
        } else if (dot && strcasecmp(dot, ".avi") == 0) {
            s_files.push_back(path);
        }
    }
    closedir(d);
    for (const std::string &s : subdirs) {
        add_dir(s, depth - 1);
    }
}

void build_list()
{
    lv_obj_clean(s_list);
    s_files.clear();
    const bool mounted = phone_storage_get(nullptr, nullptr) == PHONE_STORAGE_MOUNTED;
    if (mounted) {
        add_dir(CARD, SCAN_DEPTH);
        std::sort(s_files.begin(), s_files.end(), [](const std::string & a, const std::string & b) {
            return strcasecmp(a.c_str(), b.c_str()) < 0;
        });
    }
    for (size_t i = 0; i < s_files.size(); i++) {
        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 60);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *icon = lv_label_create(row);
        lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ACCENT), 0);
        lv_label_set_text(icon, LV_SYMBOL_VIDEO);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, lv_obj_get_width(s_list) - 24 - 64);
        lv_obj_set_height(name, lv_font_get_line_height(phone_font_by_size(20)));
        lv_label_set_text(name, name_of(s_files[i]).c_str());
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 52, 0);
    }
    if (s_files.empty()) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, 100, 0);
        lv_label_set_text(l, mounted ? T("Chưa có video.\nChép tệp .avi dạng MJPEG vào thẻ nhớ.",
                                         "No videos yet.\nCopy MJPEG .avi files to the card.")
                          : T("Không có thẻ nhớ", "No microSD card"));
    }
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
        s_img = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    const int root_w = lv_obj_get_width(root);
    const int root_h = lv_obj_get_height(root);
    s_area_w = root_w;
    s_area_h = root_h;

    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_style_text_font(title, phone_font_by_size(24), 0);
    lv_label_set_text(title, "Video");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 14);

    /* the list ends above the strip where the swipe-up for the navigation bar starts */
    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, root_w, root_h - HEADER_H - 36);
    lv_obj_set_y(s_list, HEADER_H);
    lv_obj_set_style_pad_hor(s_list, 12, 0);
    lv_obj_set_style_pad_bottom(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_update_layout(s_list);

    /* player: black page with the picture, and controls that hide while playing */
    s_player = lv_obj_create(root);
    lv_obj_remove_style_all(s_player);
    lv_obj_set_size(s_player, root_w, root_h);
    lv_obj_set_style_bg_opa(s_player, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_player, lv_color_black(), 0);
    lv_obj_remove_flag(s_player, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_player, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_event_cb(s_player, on_tap, LV_EVENT_CLICKED, nullptr);

    s_img = lv_image_create(s_player);
    lv_obj_center(s_img);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_CLICKABLE);

    s_overlay = lv_obj_create(s_player);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, root_w, root_h);
    lv_obj_remove_flag(s_overlay, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    s_title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_title, phone_font_by_size(18), 0);
    lv_obj_set_style_bg_color(s_title, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_title, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_title, 10, 0);
    lv_obj_set_style_pad_hor(s_title, 12, 0);
    lv_obj_set_style_pad_ver(s_title, 4, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(s_title, root_w - 32, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(s_title, "");

    s_message = lv_label_create(s_overlay);
    lv_obj_set_style_text_align(s_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_message, lv_color_hex(0xF87171), 0);
    lv_obj_align(s_message, LV_ALIGN_CENTER, 0, -90);
    lv_label_set_text(s_message, "");

    lv_obj_t *play = lv_button_create(s_overlay);
    lv_obj_remove_style_all(play);
    lv_obj_set_size(play, 84, 84);
    lv_obj_center(play);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(play, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_add_event_cb(play, on_play, LV_EVENT_CLICKED, nullptr);
    s_play_label = lv_label_create(play);
    lv_obj_set_style_text_font(s_play_label, &lv_font_montserrat_32, 0);
    lv_label_set_text(s_play_label, LV_SYMBOL_PAUSE);
    lv_obj_center(s_play_label);

    s_time = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_time, phone_font_by_size(16), 0);
    lv_obj_set_style_bg_color(s_time, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_time, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_time, 10, 0);
    lv_obj_set_style_pad_hor(s_time, 10, 0);
    lv_obj_set_style_pad_ver(s_time, 3, 0);
    lv_obj_align(s_time, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_label_set_text(s_time, "0:00 / 0:00");

    s_bar = lv_bar_create(s_overlay);
    lv_obj_set_size(s_bar, root_w - 48, 6);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
}

bool alloc_out()
{
    s_out_size = align_up(static_cast<size_t>(s_area_w) * s_area_h * 2);
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

PhoneVideo *PhoneVideo::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneVideo();
    }
    return _instance;
}

PhoneVideo::PhoneVideo():
    App("Video", &phone_video_icon, true, true, true)
{
}

PhoneVideo::~PhoneVideo()
{
}

bool PhoneVideo::run()
{
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
    }
    build_ui();
    if (!alloc_out()) {
        ESP_LOGE(TAG, "no memory for the picture");
        free_out();
        return false;
    }
    lv_timer_create(on_timer, 200, nullptr);
    build_list();
    return true;
}

/* Back leaves the player first, then the app */
bool PhoneVideo::back()
{
    if (s_player && !lv_obj_has_flag(s_player, LV_OBJ_FLAG_HIDDEN)) {
        close_player();
        return true;
    }
    return notifyCoreClosed();
}

bool PhoneVideo::close()
{
    stop_player();
    /* the image still points at a picture buffer until the core deletes the screen */
    if (s_img) {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_img, nullptr);
    }
    free_out();
    return true;
}

/* In the background: playback pauses */
bool PhoneVideo::pause()
{
    if (s_state == ST_PLAYING) {
        s_paused = true;
    }
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneVideo, APP_NAME, []()
{
    return std::shared_ptr<PhoneVideo>(PhoneVideo::requestInstance(), [](PhoneVideo * p) {});
})

} // namespace esp_brookesia::apps
