/*
 * Video app: plays videos from the microSD card.
 *
 * - .avi with Motion JPEG and 16-bit PCM sound: every frame is a JPEG picture, decoded by the
 *   hardware JPEG decoder, then scaled (and turned, for landscape videos) by the PPA into one of two
 *   buffers shown by an LVGL image. A player task walks the "movi" list of the AVI file in order:
 *   video chunks are shown at the frame rate of the file (late frames are dropped), audio chunks go
 *   to the speaker codec.
 * - .mp4 with H.264 constrained baseline and AAC sound: the ESP32-P4 has no H.264 decoder, so the
 *   software one of esp_h264 runs on core 1 (about 20 ms a frame at 640x360, 30 ms at 800x480).
 *   esp_extractor reads the frames; the I420 pictures are repacked for the PPA (YUV420 to RGB565,
 *   scale, turn). AAC goes through a ring buffer to a sound task. YouTube files are mostly Main or
 *   High profile and must be converted.
 *
 * Convert a video with ffmpeg:
 *   ffmpeg -i in.mp4 -vf "scale=800:480:force_original_aspect_ratio=decrease:force_divisible_by=2" -r 20
 *          -c:v mjpeg -q:v 5 -c:a pcm_s16le -ar 22050 -ac 1 out.avi
 *   ffmpeg -i in.mp4 -vf "scale=640:360:force_original_aspect_ratio=decrease:force_divisible_by=2" -r 20
 *          -c:v libx264 -profile:v baseline -level 3.1 -bf 0 -g 48 -crf 23 -pix_fmt yuv420p
 *          -c:a aac -b:a 64k -ar 22050 -ac 1 -movflags +faststart out.mp4
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
#include "phone_paths.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <sys/stat.h>
#include "esp_extractor.h"
#include "esp_extractor_defaults.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_dec_param.h"
#include "esp_aac_dec.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "VIDEO"
#include "esp_lib_utils.h"
#include "phone_ui.h"
#include "phone_audio.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"
#include "phone_quick.h"
#include "phone_storage.h"
#include "phone_app_video.hpp"

#define APP_NAME "Video"

extern "C" const lv_image_dsc_t phone_video_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_video";

constexpr const char *CARD = PHONE_SD_ROOT;
const char *const HOLD_NAME = "Video";     // holder of the card while the app runs (phone_storage_hold)
constexpr int MAX_FILES = 200;
constexpr int SCAN_DEPTH = 2;
constexpr int ALIGN = 128;                  // L2 cache line, required for PPA output buffers
constexpr int MAX_SIDE = 1920;
constexpr size_t FRAME_MAX = 2 * 1024 * 1024;
constexpr size_t AUDIO_MAX = 256 * 1024;
constexpr int HEADER_H = 60;
constexpr uint32_t OVERLAY_MS = 3000;       // the controls hide after this long
constexpr int STEP_GAP = 120;               // from the play button to the previous / next buttons
constexpr uint32_t RESTART_US = 3000000;    // previous after this much of a video: back to its start
/* Landscape videos are turned to fill the portrait screen (counter-clockwise) */
constexpr ppa_srm_rotation_angle_t LANDSCAPE_ROTATION = PPA_SRM_ROTATION_ANGLE_90;
constexpr int LANDSCAPE_TURN = 2700;        // the same turn for the labels (LVGL: 0.1 degree, clockwise)

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
lv_obj_t *s_prev_btn, *s_next_btn;     // the videos before and after it in the list
lv_obj_t *s_prev_label, *s_next_label;
int s_index = -1;           // of the open video in s_files
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
std::atomic<int> s_seek{-1};            // position asked by the progress slider, 0..1000
bool s_dragging;                        // the progress slider is held (LVGL task)
std::atomic<int> s_turned{-1};          // the picture is turned (landscape video); -1 until the first frame
bool s_wait_layout;                     // the controls stay hidden until the way of the picture is known
bool s_shown_turned;                    // the controls are laid out for a turned picture
/* Full screen while a video is open: the status bar hides and the app screen covers the whole display */
phone::StatusBar *s_status_bar;
lv_obj_t *s_screen;
int s_home_x, s_home_y;     // place of the app screen below the status bar
int s_home_w, s_home_h;
bool s_full;

/* Find the first video chunk at or after the current position of the file (a jump lands anywhere in
 * the "movi" list): its header "NNdc"/"NNdb" with a size that fits and JPEG data after it. Leaves
 * the file at that header. */
bool find_video_chunk(const Avi &avi)
{
    constexpr size_t WIN = 16 * 1024;
    auto *buf = static_cast<uint8_t *>(malloc(WIN + 12));
    if (!buf) {
        return false;
    }
    const char d0 = static_cast<char>('0' + avi.video_stream / 10), d1 = static_cast<char>('0' + avi.video_stream % 10);
    long base = ftell(avi.file);
    bool found = false;
    while (!found && base + 12 <= avi.movi_end) {
        fseek(avi.file, base, SEEK_SET);
        const size_t n = fread(buf, 1, WIN + 12, avi.file);
        if (n < 12) {
            break;
        }
        for (size_t i = 0; i + 12 <= n; i++) {
            if (buf[i] == d0 && buf[i + 1] == d1 && buf[i + 2] == 'd' && (buf[i + 3] == 'c' || buf[i + 3] == 'b') &&
                    buf[i + 8] == 0xFF && buf[i + 9] == 0xD8) {
                const uint32_t size = le32(buf + i + 4);
                if (size > 0 && size <= FRAME_MAX && base + static_cast<long>(i) + 8 + static_cast<long>(size) <= avi.movi_end) {
                    fseek(avi.file, base + static_cast<long>(i), SEEK_SET);
                    found = true;
                    break;
                }
            }
        }
        base += WIN;
    }
    free(buf);
    return found;
}

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
    bool step_one = false;          // a jump while paused: show its first frame, then hold
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
        const int seek = s_seek.exchange(-1);
        if (seek >= 0) {
            /* jump by position in the movi list, then to the next video chunk; the sound follows
             * the chunks after it. MJPEG frames all stand alone, so any of them can start. */
            const long target = avi.movi_start + static_cast<long>(static_cast<int64_t>(avi.movi_end - avi.movi_start) * seek / 1000);
            fseek(avi.file, target, SEEK_SET);
            if (seek == 0 || !find_video_chunk(avi)) {
                fseek(avi.file, seek == 0 ? avi.movi_start : avi.movi_end, SEEK_SET);
            }
            frame = static_cast<uint32_t>(static_cast<uint64_t>(avi.total_frames) * seek / 1000);
            s_frame = frame;
            start_us = esp_timer_get_time() - static_cast<int64_t>(frame) * avi.usec_per_frame;
            step_one = s_paused;
        }
        if ((s_paused && !step_one) || phone_lock_is_locked()) {
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
            /* more than two frames late, or under the quick settings panel (a new picture would redraw the
             * whole panel): drop this one */
            const bool drop = now - due > 2 * static_cast<int64_t>(avi.usec_per_frame) || phone_quick_visible();
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
                            s_turned = turned ? 1 : 0;
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
                            step_one = false;
                        }
                        bad_frames = 0;
                    } else if (++bad_frames > 10) {
                        ESP_LOGE(TAG, "frames cannot be decoded: not baseline MJPEG?");
                        ok = false;
                        break;
                    }
                }
            }
        } else if (is_audio && spk_open && !step_one && size > 0 && size <= AUDIO_MAX) {
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

/* ---- MP4: H.264 constrained baseline (software decoder) + AAC ---- */

constexpr uint32_t MP4_POOL = 512 * 1024;       // extractor output pool: the largest frame must fit
constexpr size_t MP4_AUDIO_RING = 64 * 1024;    // AAC packets waiting for the sound task (several seconds)
constexpr int MP4_BAD_FRAMES = 30;              // frames in a row without a picture: not a stream we can decode

int mp4_read(void *buf, uint32_t size, void *ctx)
{
    return static_cast<int>(fread(buf, 1, size, static_cast<FILE *>(ctx)));
}

int mp4_seek(uint32_t pos, void *ctx)
{
    return fseek(static_cast<FILE *>(ctx), pos, SEEK_SET) == 0 ? 0 : -1;
}

uint32_t mp4_size(void *ctx)
{
    struct stat st;
    return fstat(fileno(static_cast<FILE *>(ctx)), &st) == 0 ? static_cast<uint32_t>(st.st_size) : 0;
}

bool is_annex_b(const uint8_t *b, size_t n)
{
    return (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) || (n >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1);
}

/* SPS and PPS of an avcC box as Annex B; returns the size written */
size_t avcc_header(const uint8_t *c, size_t n, uint8_t *out, size_t cap)
{
    if (n < 7 || c[0] != 1) {
        return 0;
    }
    size_t w = 0, p = 5;
    for (int list = 0; list < 2 && p < n; list++) {
        const int count = list == 0 ? (c[p] & 0x1F) : c[p];
        p++;
        for (int i = 0; i < count && p + 2 <= n; i++) {
            const size_t len = (c[p] << 8) | c[p + 1];
            p += 2;
            if (p + len > n || w + 4 + len > cap) {
                return w;
            }
            static const uint8_t start[4] = {0, 0, 0, 1};
            memcpy(out + w, start, 4);
            memcpy(out + w + 4, c + p, len);
            w += 4 + len;
            p += len;
        }
    }
    return w;
}

/* 4-byte NAL lengths turned into start codes, in place */
void avcc_to_annex_b(uint8_t *b, size_t n)
{
    for (size_t p = 0; p + 4 <= n;) {
        const size_t len = (static_cast<size_t>(b[p]) << 24) | (b[p + 1] << 16) | (b[p + 2] << 8) | b[p + 3];
        b[p] = b[p + 1] = b[p + 2] = 0;
        b[p + 3] = 1;
        p += 4 + len;
    }
}

/* I420 (planes Y, U, V) to the YUV420 of the PPA (lines "U Y Y ..." and "V Y Y ..."). w and h even;
 * `stride` is the width of the decoded picture. */
void i420_to_ppa(const uint8_t *src, int stride, int stride_h, int w, int h, uint8_t *dst)
{
    const uint8_t *py = src;
    const uint8_t *pu = src + stride * stride_h;
    const uint8_t *pv = pu + (stride / 2) * (stride_h / 2);
    const int line = w * 3 / 2;
    for (int k = 0; k < h / 2; k++) {
        const uint8_t *y0 = py + 2 * k * stride;
        const uint8_t *y1 = y0 + stride;
        const uint8_t *u = pu + k * (stride / 2);
        const uint8_t *v = pv + k * (stride / 2);
        uint8_t *o0 = dst + 2 * k * line;
        uint8_t *o1 = o0 + line;
        for (int x = 0; x < w / 2; x++) {
            o0[0] = u[x];
            o0[1] = y0[0];
            o0[2] = y0[1];
            o1[0] = v[x];
            o1[1] = y1[0];
            o1[2] = y1[1];
            o0 += 3;
            o1 += 3;
            y0 += 2;
            y1 += 2;
        }
    }
}

/* Sound of an MP4: the player task sends AAC packets (a generation number first, so a jump drops the
 * old ones); this task decodes them and plays them, paced by the speaker */
struct Mp4Sound {
    RingbufHandle_t ring;
    esp_codec_dev_handle_t spk;
    uint32_t rate;
    uint8_t channels;
    std::atomic<uint32_t> gen;
    std::atomic<bool> quit;
    SemaphoreHandle_t done;
};

void mp4_sound_task(void *arg)
{
    auto *snd = static_cast<Mp4Sound *>(arg);
    void *dec = nullptr;
    bool dec_failed = false;
    uint32_t pcm_size = 8192;
    auto *pcm = static_cast<uint8_t *>(malloc(pcm_size));
    uint32_t gen_seen = 0, open_rate = 0;
    int volume = -1;
    while (!snd->quit) {
        if (s_paused || phone_lock_is_locked()) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        size_t n = 0;
        auto *item = static_cast<uint8_t *>(xRingbufferReceive(snd->ring, &n, pdMS_TO_TICKS(40)));
        if (!item) {
            continue;
        }
        uint32_t gen;
        memcpy(&gen, item, 4);
        if (!dec && !dec_failed && n > 6) {
            /* esp_extractor puts an ADTS header on the AAC frames of an MP4; raw frames need the format */
            esp_aac_dec_cfg_t cfg = {};
            cfg.sample_rate = static_cast<int32_t>(snd->rate);
            cfg.channel = snd->channels;
            cfg.bits_per_sample = 16;
            cfg.no_adts_header = !(item[4] == 0xFF && (item[5] & 0xF0) == 0xF0);
            cfg.aac_plus_enable = true;
            if (esp_aac_dec_open(&cfg, sizeof(cfg), &dec) != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "AAC decoder");
                dec = nullptr;
                dec_failed = true;
            } else {
                ESP_LOGI(TAG, "AAC %s ADTS header", cfg.no_adts_header ? "without" : "with");
            }
        }
        if (gen == snd->gen && dec && pcm && n > 4) {
            if (gen != gen_seen) {
                esp_aac_dec_reset(dec);
                gen_seen = gen;
            }
            esp_audio_dec_in_raw_t in = {};
            in.buffer = item + 4;
            in.len = n - 4;
            while (in.len) {
                esp_audio_dec_out_frame_t out = {};
                out.buffer = pcm;
                out.len = pcm_size;
                esp_audio_dec_info_t info = {};
                const esp_audio_err_t ret = esp_aac_dec_decode(dec, &in, &out, &info);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > pcm_size) {
                    auto *bigger = static_cast<uint8_t *>(realloc(pcm, out.needed_size));
                    if (!bigger) {
                        break;
                    }
                    pcm = bigger;
                    pcm_size = out.needed_size;
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    break;
                }
                if (out.decoded_size && info.sample_rate && info.channel) {
                    if (info.sample_rate != open_rate) {
                        /* the rate of the stream (AAC-Plus doubles the one of the file) */
                        if (open_rate) {
                            esp_codec_dev_close(snd->spk);
                        }
                        esp_codec_dev_sample_info_t fs = {};
                        fs.sample_rate = info.sample_rate;
                        fs.channel = 1;
                        fs.bits_per_sample = 16;
                        open_rate = esp_codec_dev_open(snd->spk, &fs) == ESP_CODEC_DEV_OK ? info.sample_rate : 0;
                        volume = -1;
                    }
                    auto *s = reinterpret_cast<int16_t *>(pcm);
                    const size_t frames = out.decoded_size / 2 / info.channel;
                    if (info.channel == 2) {
                        for (size_t i = 0; i < frames; i++) {
                            s[i] = static_cast<int16_t>((s[2 * i] + s[2 * i + 1]) / 2);
                        }
                    }
                    const int v = phone_audio_get_volume();
                    if (v != volume && open_rate) {
                        volume = v;
                        esp_codec_dev_set_out_vol(snd->spk, v);
                    }
                    if (open_rate) {
                        esp_codec_dev_write(snd->spk, s, frames * 2);
                    }
                }
                if (!in.consumed) {
                    break;
                }
                in.buffer += in.consumed;
                in.len -= in.consumed;
            }
        }
        vRingbufferReturnItem(snd->ring, item);
    }
    if (open_rate) {
        esp_codec_dev_close(snd->spk);
    }
    if (dec) {
        esp_aac_dec_close(dec);
    }
    free(pcm);
    xSemaphoreGive(snd->done);
    vTaskDeleteWithCaps(nullptr);
}

void mp4_player_task(void *)
{
    FILE *f = nullptr;
    esp_extractor_handle_t ex = nullptr;
    esp_h264_dec_handle_t dec = nullptr;
    ppa_client_handle_t ppa = nullptr;
    Mp4Sound snd = {};
    bool snd_running = false;
    uint8_t *in = nullptr, *yuv = nullptr;
    size_t in_cap = 0, yuv_size = 0;
    uint8_t head[256];
    size_t head_len = 0;
    bool need_head = true, annex_b = true, first_frame = true;
    bool resync = true;             // the clock follows the next picture (start, jump)
    bool step_one = false;          // a jump while paused: show its first picture, then hold
    int dec_w = 0, dec_h = 0, pic_w = 0, pic_h = 0, out_w = 0, out_h = 0, steps = 16;
    bool turned = false;
    int back = 0, bad_frames = 0;
    uint32_t upf = 40000;
    uint32_t duration_ms = 0;
    int64_t start_us = 0;
    uint32_t pictures = 0, dropped = 0;
    int64_t t_dec = 0;
    bool ok = false;
    const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};
    esp_extractor_stream_info_t vinfo = {}, ainfo = {};
    uint16_t audio_streams = 0;

    static bool registered;
    if (!registered) {
        esp_mp4_extractor_register();
        registered = true;
    }
    f = fopen(s_path.c_str(), "rb");
    if (!f) {
        goto done;
    }
    setvbuf(f, nullptr, _IOFBF, 32 * 1024);
    {
        esp_extractor_config_t cfg = {};
        cfg.type = ESP_EXTRACTOR_TYPE_MP4;
        cfg.extract_mask = ESP_EXTRACT_MASK_AV;
        cfg.in_read_cb = mp4_read;
        cfg.in_seek_cb = mp4_seek;
        cfg.in_size_cb = mp4_size;
        cfg.in_ctx = f;
        cfg.out_pool_size = MP4_POOL;
        cfg.out_align = 128;
        if (esp_extractor_open(&cfg, &ex) != ESP_EXTRACTOR_ERR_OK || esp_extractor_parse_stream(ex) != ESP_EXTRACTOR_ERR_OK ||
                esp_extractor_get_stream_info(ex, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, 0, &vinfo) != ESP_EXTRACTOR_ERR_OK ||
                vinfo.video_info.format != ESP_EXTRACTOR_VIDEO_FORMAT_H264) {
            ESP_LOGE(TAG, "%s: no H.264 video", s_path.c_str());
            goto done;
        }
    }
    if (esp_extractor_get_stream_num(ex, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, &audio_streams) == ESP_EXTRACTOR_ERR_OK &&
            audio_streams > 0) {
        esp_extractor_get_stream_info(ex, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, 0, &ainfo);
    }
    upf = vinfo.video_info.fps ? 1000000u / vinfo.video_info.fps : 40000u;
    duration_ms = vinfo.duration;
    s_usec_per_frame = upf;
    s_total = static_cast<uint32_t>(static_cast<uint64_t>(duration_ms) * 1000 / upf);
    ESP_LOGI(TAG, "%s: H.264 %ux%u %u fps, %u ms, audio %.4s %u Hz %u ch", s_path.c_str(), vinfo.video_info.width,
             vinfo.video_info.height, vinfo.video_info.fps, static_cast<unsigned>(duration_ms),
             audio_streams ? reinterpret_cast<const char *>(&ainfo.audio_info.format) : "none",
             static_cast<unsigned>(ainfo.audio_info.sample_rate), ainfo.audio_info.channel);
    if (vinfo.spec_info && vinfo.spec_info_len) {
        if (is_annex_b(vinfo.spec_info, vinfo.spec_info_len)) {
            head_len = std::min<size_t>(vinfo.spec_info_len, sizeof(head));
            memcpy(head, vinfo.spec_info, head_len);
        } else {
            head_len = avcc_header(vinfo.spec_info, vinfo.spec_info_len, head, sizeof(head));
        }
    }
    {
        esp_h264_dec_cfg_sw_t dcfg = {};
        dcfg.pic_type = ESP_H264_RAW_FMT_I420;
        if (esp_h264_dec_sw_new(&dcfg, &dec) != ESP_H264_ERR_OK || esp_h264_dec_open(dec) != ESP_H264_ERR_OK ||
                ppa_register_client(&ppa_cfg, &ppa) != ESP_OK) {
            goto done;
        }
    }

    /* sound: AAC only; the speaker may be busy (music) */
    if (audio_streams && ainfo.audio_info.format == ESP_EXTRACTOR_AUDIO_FORMAT_AAC && ainfo.audio_info.sample_rate &&
            (ainfo.audio_info.channel == 1 || ainfo.audio_info.channel == 2)) {
        snd.spk = static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take());
        if (snd.spk) {
            snd.rate = ainfo.audio_info.sample_rate;
            snd.channels = ainfo.audio_info.channel;
            snd.ring = xRingbufferCreateWithCaps(MP4_AUDIO_RING, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
            snd.done = xSemaphoreCreateBinary();
            snd_running = snd.ring && snd.done &&
                          xTaskCreatePinnedToCoreWithCaps(mp4_sound_task, "mp4_sound", 8192, &snd, 5, nullptr, 0,
                                                          MALLOC_CAP_SPIRAM) == pdPASS;
            if (!snd_running) {
                ESP_LOGE(TAG, "no sound task");
            }
        }
    }
    if (!snd_running) {
        esp_extractor_enable_stream(ex, ESP_EXTRACTOR_STREAM_TYPE_AUDIO, 0, false);
    }

    ok = true;
    s_state = ST_PLAYING;
    while (!s_stop) {
        const int seek = s_seek.exchange(-1);
        if (seek >= 0) {
            /* the extractor starts again at the key frame at or before the time */
            const uint32_t t = static_cast<uint32_t>(static_cast<uint64_t>(duration_ms) * seek / 1000);
            if (esp_extractor_seek(ex, t) == ESP_EXTRACTOR_ERR_OK) {
                snd.gen++;
                resync = true;
                need_head = true;
                step_one = s_paused;
                s_frame = static_cast<uint32_t>(static_cast<uint64_t>(t) * 1000 / upf);
            }
        }
        if ((s_paused && !step_one) || phone_lock_is_locked()) {
            /* hold the clock while paused or while the lock screen is up */
            const int64_t t0 = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(40));
            start_us += esp_timer_get_time() - t0;
            continue;
        }
        esp_extractor_frame_info_t fr = {};
        const int ret = esp_extractor_read_frame(ex, &fr);
        if (ret == ESP_EXTRACTOR_ERR_SKIPPED || ret == ESP_EXTRACTOR_ERR_WAITING_OUTPUT) {
            continue;
        }
        if (ret != ESP_EXTRACTOR_ERR_OK) {
            if (ret != ESP_EXTRACTOR_ERR_EOS) {
                ESP_LOGE(TAG, "read frame: %d", ret);
            }
            break;
        }
        if (fr.stream_type == ESP_EXTRACTOR_STREAM_TYPE_AUDIO) {
            if (snd_running && !step_one && fr.frame_size) {
                void *item = nullptr;
                if (xRingbufferSendAcquire(snd.ring, &item, fr.frame_size + 4, pdMS_TO_TICKS(200)) == pdTRUE) {
                    const uint32_t gen = snd.gen;
                    memcpy(item, &gen, 4);
                    memcpy(static_cast<uint8_t *>(item) + 4, fr.frame_buffer, fr.frame_size);
                    xRingbufferSendComplete(snd.ring, item);
                }
            }
            esp_extractor_release_frame(ex, &fr);
            continue;
        }
        if (fr.stream_type != ESP_EXTRACTOR_STREAM_TYPE_VIDEO || !fr.frame_size) {
            const bool eos = EXTRACTOR_IS_EOS(fr.frame_flag);
            esp_extractor_release_frame(ex, &fr);
            if (eos) {
                break;
            }
            continue;
        }

        /* a copy: AVCC lengths become start codes, SPS/PPS go in front after a start or a jump */
        if (first_frame) {
            annex_b = is_annex_b(fr.frame_buffer, fr.frame_size);
            first_frame = false;
        }
        const size_t need = fr.frame_size + (need_head ? head_len : 0);
        if (need > in_cap) {
            heap_caps_free(in);
            in_cap = need + 64 * 1024;
            in = static_cast<uint8_t *>(heap_caps_malloc(in_cap, MALLOC_CAP_SPIRAM));
            if (!in) {
                in_cap = 0;
                esp_extractor_release_frame(ex, &fr);
                ok = false;
                break;
            }
        }
        size_t n = 0;
        if (need_head) {
            memcpy(in, head, head_len);
            n = head_len;
            need_head = false;
        }
        memcpy(in + n, fr.frame_buffer, fr.frame_size);
        if (!annex_b) {
            avcc_to_annex_b(in + n, fr.frame_size);
        }
        n += fr.frame_size;
        const uint32_t pts = fr.pts;
        esp_extractor_release_frame(ex, &fr);

        /* decode every frame (the next ones refer to it), show it only when in time */
        esp_h264_dec_in_frame_t din = {};
        din.raw_data.buffer = in;
        din.raw_data.len = n;
        uint8_t *picture = nullptr;
        uint32_t picture_size = 0;
        const int64_t t0 = esp_timer_get_time();
        while (din.raw_data.len) {
            esp_h264_dec_out_frame_t dout = {};
            if (esp_h264_dec_process(dec, &din, &dout) != ESP_H264_ERR_OK) {
                break;
            }
            if (dout.out_size) {
                picture = dout.outbuf;
                picture_size = dout.out_size;
            }
            if (!din.consume) {
                break;
            }
            din.raw_data.buffer += din.consume;
            din.raw_data.len -= din.consume;
        }
        t_dec += esp_timer_get_time() - t0;
        if (!picture) {
            if (++bad_frames > MP4_BAD_FRAMES) {
                ESP_LOGE(TAG, "frames cannot be decoded: not constrained baseline H.264?");
                ok = false;
                break;
            }
            continue;
        }
        bad_frames = 0;
        pictures++;

        if (!yuv) {
            /* size of the decoded picture (rows padded to 16), the picture itself, the fit on the screen */
            esp_h264_dec_param_sw_handle_t param = nullptr;
            esp_h264_resolution_t res = {};
            if (esp_h264_dec_sw_get_param_hd(dec, &param) == ESP_H264_ERR_OK &&
                    esp_h264_dec_get_resolution(param, &res) == ESP_H264_ERR_OK && res.width && res.height) {
                dec_w = res.width;
                dec_h = res.height;
            } else {
                dec_w = vinfo.video_info.width;
                dec_h = picture_size * 2 / 3 / dec_w;
            }
            pic_w = std::min<int>(vinfo.video_info.width ? vinfo.video_info.width : dec_w, dec_w) & ~1;
            pic_h = std::min<int>(vinfo.video_info.height ? vinfo.video_info.height : dec_h, dec_h) & ~1;
            yuv_size = align_up(static_cast<size_t>(pic_w) * pic_h * 3 / 2);
            yuv = static_cast<uint8_t *>(heap_caps_aligned_calloc(ALIGN, 1, yuv_size, MALLOC_CAP_SPIRAM));
            if (!yuv || static_cast<size_t>(dec_w) * dec_h * 3 / 2 > picture_size) {
                ESP_LOGE(TAG, "picture %dx%d, %u bytes: no memory or wrong size", dec_w, dec_h, static_cast<unsigned>(picture_size));
                ok = false;
                break;
            }
            turned = pic_w > pic_h;
            s_turned = turned ? 1 : 0;
            const int rw = turned ? pic_h : pic_w;
            const int rh = turned ? pic_w : pic_h;
            steps = std::clamp(std::min(s_area_w * 16 / rw, s_area_h * 16 / rh), 1, 48);
            out_w = rw * steps / 16;
            out_h = rh * steps / 16;
            ESP_LOGI(TAG, "picture %dx%d (decoded %dx%d), shown %dx%d", pic_w, pic_h, dec_w, dec_h, out_w, out_h);
        }

        const int64_t now = esp_timer_get_time();
        if (resync) {
            start_us = now - static_cast<int64_t>(pts) * 1000;
            resync = false;
        }
        const int64_t due = start_us + static_cast<int64_t>(pts) * 1000;
        s_frame = static_cast<uint32_t>(static_cast<uint64_t>(pts) * 1000 / upf);
        if (phone_quick_visible() && !step_one) {
            continue;           /* under the quick settings panel: decoded for the next frames, not shown */
        }
        if (now - due > 2 * static_cast<int64_t>(upf) && !step_one) {
            dropped++;          /* late: decoded for the next frames, not shown */
            continue;
        }
        i420_to_ppa(picture, dec_w, dec_h, pic_w, pic_h, yuv);
        ppa_srm_oper_config_t op = {};
        op.in.buffer = yuv;
        op.in.pic_w = pic_w;
        op.in.pic_h = pic_h;
        op.in.block_w = pic_w;
        op.in.block_h = pic_h;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
        op.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
        op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
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
            const int64_t wait = due - esp_timer_get_time();
            if (wait > 1000 && !step_one) {
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
            step_one = false;
        }
    }
    ESP_LOGI(TAG, "MP4 done: %u pictures, %u late, decode avg %.1f ms, stack left %u", static_cast<unsigned>(pictures),
             static_cast<unsigned>(dropped), pictures ? t_dec / 1000.0 / pictures : 0.0,
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

done:
    if (snd_running) {
        snd.quit = true;
        xSemaphoreTake(snd.done, portMAX_DELAY);
    }
    if (snd.spk) {
        phone_audio_speaker_give();
    }
    if (snd.ring) {
        vRingbufferDeleteWithCaps(snd.ring);
    }
    if (snd.done) {
        vSemaphoreDelete(snd.done);
    }
    if (ppa) {
        ppa_unregister_client(ppa);
    }
    if (dec) {
        esp_h264_dec_close(dec);
        esp_h264_dec_del(dec);
    }
    if (ex) {
        esp_extractor_close(ex);
    }
    if (f) {
        fclose(f);
    }
    heap_caps_free(in);
    heap_caps_free(yuv);
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

/* Turn a label with the picture, about its own center */
void turn_label(lv_obj_t *label, bool turned)
{
    lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(label, LV_PCT(50), 0);
    lv_obj_set_style_transform_rotation(label, turned ? LANDSCAPE_TURN : 0, 0);
}

/* Lays the controls out for the way the picture is seen. A turned picture is watched with the phone
 * turned clockwise: the top of the video is the left edge of the screen and its bottom the right edge,
 * so the progress slider stands along the right edge and runs upwards (as an LVGL vertical slider does).
 * Only the labels are turned; the play button is round and the slider vertical, so touches need no
 * transform. */
void layout_controls(bool turned)
{
    s_shown_turned = turned;
    const int half_w = s_area_w / 2;
    const int title_h = lv_font_get_line_height(phone_font_by_size(18)) + 8;
    const int time_h = lv_font_get_line_height(phone_font_by_size(16)) + 6;
    turn_label(s_title, turned);
    turn_label(s_time, turned);
    turn_label(s_message, turned);
    turn_label(s_play_label, turned);
    turn_label(s_prev_label, turned);
    turn_label(s_next_label, turned);
    /* the right of a turned picture is the top of the screen */
    lv_obj_align(s_prev_btn, LV_ALIGN_CENTER, turned ? 0 : -STEP_GAP, turned ? STEP_GAP : 0);
    lv_obj_align(s_next_btn, LV_ALIGN_CENTER, turned ? 0 : STEP_GAP, turned ? -STEP_GAP : 0);
    if (turned) {
        lv_obj_set_style_max_width(s_title, s_area_h - 32, 0);
        lv_obj_align(s_title, LV_ALIGN_CENTER, -half_w + 12 + title_h / 2, 0);
        lv_obj_align(s_message, LV_ALIGN_CENTER, -90, 0);
        lv_obj_align(s_time, LV_ALIGN_CENTER, half_w - 64 - time_h / 2, 0);
        lv_obj_set_size(s_bar, 6, s_area_h - 48);
        lv_obj_align(s_bar, LV_ALIGN_RIGHT_MID, -48, 0);
    } else {
        lv_obj_set_style_max_width(s_title, s_area_w - 32, 0);
        lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_align(s_message, LV_ALIGN_CENTER, 0, -90);
        lv_obj_align(s_time, LV_ALIGN_BOTTOM_MID, 0, -64);
        lv_obj_set_size(s_bar, s_area_w - 48, 6);
        lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -48);
    }
}

void set_full(bool on)
{
    if (!s_screen || on == s_full) {
        return;
    }
    s_full = on;
    if (s_status_bar) {
        s_status_bar->setVisualMode(on ? phone::StatusBar::VisualMode::HIDE : phone::StatusBar::VisualMode::SHOW_FIXED);
    }
    /* LVGL never resizes a screen (lv_obj_refr_size() skips objects without a parent) and draws nothing
     * of it outside its area, so the area of the screen is set here. The core created it with the size
     * of the visual area below the status bar. */
    if (on) {
        s_home_x = lv_obj_get_x(s_screen);
        s_home_y = lv_obj_get_y(s_screen);
        lv_obj_set_pos(s_screen, 0, 0);
        s_area_w = BSP_LCD_H_RES;
        s_area_h = BSP_LCD_V_RES;
    } else {
        lv_obj_set_pos(s_screen, s_home_x, s_home_y);
        s_area_w = s_home_w;
        s_area_h = s_home_h;
    }
    s_screen->coords.x2 = s_screen->coords.x1 + s_area_w - 1;
    s_screen->coords.y2 = s_screen->coords.y1 + s_area_h - 1;
    lv_obj_mark_layout_as_dirty(s_root);         // its size is 100 % of the screen
    lv_obj_invalidate(s_screen);
    lv_obj_update_layout(s_screen);
    layout_controls(s_shown_turned);
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
    set_full(false);
}

void open_video(int index)
{
    if (index < 0 || index >= static_cast<int>(s_files.size()) || s_task) {
        return;
    }
    s_path = s_files[index];
    s_index = index;
    lv_obj_set_state(s_prev_btn, LV_STATE_DISABLED, false);      // previous also goes back to the start
    lv_obj_set_state(s_next_btn, LV_STATE_DISABLED, index + 1 >= static_cast<int>(s_files.size()));
    s_stop = false;
    s_paused = false;
    s_frame = 0;
    s_total = 0;
    s_shown_state = -1;
    lv_label_set_text(s_title, name_of(s_path).c_str());
    lv_label_set_text(s_message, "");
    lv_slider_set_value(s_bar, 0, LV_ANIM_OFF);
    s_seek = -1;
    s_dragging = false;
    s_turned = -1;
    lv_image_set_src(s_img, nullptr);
    lv_obj_remove_flag(s_player, LV_OBJ_FLAG_HIDDEN);
    set_full(true);             // before the player task reads the size of the picture area
    /* the controls show once the first frame tells which way they go (on_timer) */
    s_wait_layout = true;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_state = ST_IDLE;
    const char *dot = strrchr(s_path.c_str(), '.');
    const bool mp4 = dot && strcasecmp(dot, ".mp4") == 0;
    /* MP4: the H.264 decoder takes most of core 1; LVGL runs on core 0 */
    if ((mp4 ? xTaskCreatePinnedToCore(mp4_player_task, "phone_mp4", 8192, nullptr, 4, &s_task, 1)
             : xTaskCreate(player_task, "phone_video", 8192, nullptr, 4, &s_task)) != pdPASS) {
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

/* Previous / next video in the list. Previous goes back to the start of the video first once it has
 * played a while, as phones do. */
void on_step(lv_event_t *e)
{
    const int step = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (step < 0 && s_state == ST_PLAYING &&
            static_cast<uint64_t>(s_frame) * s_usec_per_frame >= RESTART_US) {
        s_seek = 0;
        show_overlay();
        return;
    }
    const int index = s_index + step;
    if (index < 0 || index >= static_cast<int>(s_files.size())) {
        show_overlay();
        return;
    }
    stop_player();
    lv_image_set_src(s_img, nullptr);
    open_video(index);
}

/* Progress slider: jumps when it is let go */
void on_seek(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_dragging = true;
        show_overlay();
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_dragging) {
        s_dragging = false;
        if (s_state == ST_PLAYING) {
            s_seek = lv_slider_get_value(s_bar);
        }
        show_overlay();
    }
}

/* A tap on the picture shows or hides the controls */
void on_tap(lv_event_t *)
{
    if (s_wait_layout) {
        return;
    }
    if (lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        show_overlay();
    } else {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/* m:ss, or h:mm:ss from one hour on */
void format_time(char *buf, size_t len, uint32_t seconds)
{
    if (seconds >= 3600) {
        snprintf(buf, len, "%u:%02u:%02u", static_cast<unsigned>(seconds / 3600), static_cast<unsigned>(seconds / 60 % 60),
                 static_cast<unsigned>(seconds % 60));
    } else {
        snprintf(buf, len, "%u:%02u", static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
    }
}

void show_time(uint32_t at, uint32_t all)
{
    char a[16], b[16];
    format_time(a, sizeof(a), at);
    format_time(b, sizeof(b), all);
    lv_label_set_text_fmt(s_time, "%s / %s", a, b);
}

void on_timer(lv_timer_t *)
{
    if (!s_root || lv_obj_has_flag(s_player, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    const int state = s_state;
    const bool paused = s_paused;
    if (!s_full && lv_obj_get_screen(s_root) == lv_screen_active()) {
        set_full(true);         // back from the background (the core showed the status bar again)
    }
    const int turned = s_turned;
    if (s_wait_layout) {
        if (turned >= 0 || state == ST_ERROR) {
            s_wait_layout = false;
            layout_controls(turned > 0);
            show_overlay();
        }
    } else if (turned >= 0 && (turned > 0) != s_shown_turned) {
        layout_controls(turned > 0);
    }
    if (state != s_shown_state || paused != s_shown_paused) {
        s_shown_state = state;
        s_shown_paused = paused;
        lv_label_set_text(s_play_label, state == ST_PLAYING && !paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        if (state == ST_ERROR) {
            lv_label_set_text(s_message, T("Không phát được video này.\nCần .avi MJPEG hoặc\n.mp4 H.264 baseline.",
                                           "Cannot play this video.\nIt must be an MJPEG .avi or a\nbaseline H.264 .mp4 file."));
            show_overlay();
        } else if (state == ST_ENDED) {
            /* the next video of the list plays on; after the last one the list comes back */
            if (s_index + 1 < static_cast<int>(s_files.size())) {
                stop_player();
                open_video(s_index + 1);
            } else {
                close_player();
            }
            return;
        }
    }
    const uint32_t total = s_total;
    const uint32_t frame = s_frame;
    const uint32_t upf = s_usec_per_frame;
    const uint32_t sec = static_cast<uint32_t>(static_cast<uint64_t>(frame) * upf / 1000000);
    const uint32_t all = static_cast<uint32_t>(static_cast<uint64_t>(total) * upf / 1000000);
    show_time(sec, all);
    if (!s_dragging && s_seek < 0) {
        lv_slider_set_value(s_bar, total ? static_cast<int>(static_cast<uint64_t>(frame) * 1000 / total) : 0, LV_ANIM_OFF);
    } else if (s_dragging) {
        /* the time under the finger */
        const uint32_t at = static_cast<uint32_t>(static_cast<uint64_t>(all) * lv_slider_get_value(s_bar) / 1000);
        show_time(at, all);
        show_overlay();
    }

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
        } else if (dot && (strcasecmp(dot, ".avi") == 0 || strcasecmp(dot, ".mp4") == 0)) {
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
        s_screen = nullptr;
        s_full = false;
    }, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    const int root_w = lv_obj_get_width(root);
    const int root_h = lv_obj_get_height(root);
    s_area_w = s_home_w = root_w;
    s_area_h = s_home_h = root_h;
    s_screen = lv_obj_get_screen(root);
    s_full = false;

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
    phone_ui_scrollbar(s_list);
    lv_obj_update_layout(s_list);

    /* player: black page with the picture, and controls that hide while playing */
    s_player = lv_obj_create(root);
    lv_obj_remove_style_all(s_player);
    lv_obj_set_size(s_player, LV_PCT(100), LV_PCT(100));
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
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(s_overlay, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    s_title = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_title, phone_font_by_size(18), 0);
    lv_obj_set_style_bg_color(s_title, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_title, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_title, 10, 0);
    lv_obj_set_style_pad_hor(s_title, 12, 0);
    lv_obj_set_style_pad_ver(s_title, 4, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(s_title, "");

    s_message = lv_label_create(s_overlay);
    lv_obj_set_style_text_align(s_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_message, lv_color_hex(0xF87171), 0);
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

    auto step_button = [](lv_obj_t *parent, const char *symbol, int step, lv_obj_t **label) {
        lv_obj_t *btn = lv_button_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 60, 60);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
        lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_set_ext_click_area(btn, 10);
        lv_obj_add_event_cb(btn, on_step, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(step)));
        *label = lv_label_create(btn);
        lv_obj_set_style_text_font(*label, &lv_font_montserrat_24, 0);
        lv_label_set_text(*label, symbol);
        lv_obj_center(*label);
        return btn;
    };
    s_prev_btn = step_button(s_overlay, LV_SYMBOL_PREV, -1, &s_prev_label);
    s_next_btn = step_button(s_overlay, LV_SYMBOL_NEXT, 1, &s_next_label);

    s_time = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_time, phone_font_by_size(16), 0);
    lv_obj_set_style_bg_color(s_time, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_time, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_time, 10, 0);
    lv_obj_set_style_pad_hor(s_time, 10, 0);
    lv_obj_set_style_pad_ver(s_time, 3, 0);
    lv_label_set_text(s_time, "0:00 / 0:00");

    s_bar = lv_slider_create(s_overlay);
    lv_slider_set_range(s_bar, 0, 1000);
    lv_obj_set_ext_click_area(s_bar, 16);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_bar, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(s_bar, on_seek, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_bar, on_seek, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_bar, on_seek, LV_EVENT_PRESS_LOST, nullptr);
    layout_controls(false);
}

bool alloc_out()
{
    s_out_size = align_up(static_cast<size_t>(BSP_LCD_H_RES) * BSP_LCD_V_RES * 2);     // full screen
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

/* The player task, for phone_video_h264mem.cpp */
extern "C" TaskHandle_t phone_video_task(void)
{
    return s_task;
}

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
    phone_storage_hold(HOLD_NAME);          /* a paused video keeps its file open */
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
    }
    s_status_bar = getSystem()->getDisplay().getStatusBar();
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
    phone_storage_release(HOLD_NAME);
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
    set_full(false);
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
