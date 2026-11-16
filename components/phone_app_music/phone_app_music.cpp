/*
 * Music app: plays the audio files of the microSD card (MP3, WAV, FLAC, AAC, M4A, OGG) on the
 * speaker. Files are taken from the card and up to three folder levels below its top.
 *
 * - music::Service (music/, from esp_chatbot_v1) lists the files and reads the ID3 tag and the
 *   duration of the MP3 files in a background thread: the track that is playing first, then the
 *   rows on the screen, and nothing while the list is being scrolled.
 * - The cover is the JPEG picture of the ID3v2 tag, when there is one: decoded by the hardware
 *   JPEG decoder and scaled by the PPA before the track starts. Otherwise a colored placeholder.
 * - A player task reads the file, decodes it with esp_audio_codec and writes mono 16-bit PCM to
 *   the speaker codec. It keeps playing while the app is in the background and while the screen
 *   is off; closing the app stops it. The volume is the phone volume, followed while playing.
 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <strings.h>
#include <vector>
#include "decoder/esp_audio_dec_default.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "MUSIC"
#include "esp_lib_utils.h"
#include "music_service.hpp"
#include "phone_audio.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_storage.h"
#include "phone_app_music.hpp"

#define APP_NAME "Nhạc"

extern "C" const lv_image_dsc_t phone_music_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_music";

constexpr const char *CARD = "/sdcard";
constexpr size_t IN_SIZE = 4096;
constexpr size_t PCM_SIZE = 16 * 1024;
constexpr int CARD_H = 392;         // now-playing card
constexpr int ART = 150;            // artwork placeholder
constexpr int ROW_H = 60;
constexpr int ROW_GAP = 6;

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_ROW = 0x1E252C;
constexpr uint32_t COLOR_ROW_CUR = 0x3A2030;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0xFF5F78;

/* Artwork placeholder: gradient and two discs, one palette per track (from esp_chatbot_v1) */
struct Palette {
    uint32_t start, end, primary, secondary;
};
const Palette PALETTES[] = {
    {0xFF6B35, 0xFFB347, 0xFFD166, 0xEF476F}, {0x253C78, 0x6A4C93, 0x4CC9F0, 0xF72585},
    {0x111827, 0x30475E, 0x8B5CF6, 0x22D3EE}, {0x075985, 0x0E7490, 0x67E8F9, 0x38BDF8},
    {0x92400E, 0xF59E0B, 0xFDE68A, 0xFB7185}, {0x312E81, 0x7C3AED, 0xC4B5FD, 0xF472B6},
    {0x14532D, 0x15803D, 0x86EFAC, 0xFDE047}, {0x7F1D1D, 0xBE123C, 0xFDA4AF, 0xFDBA74},
};
constexpr int PALETTE_COUNT = sizeof(PALETTES) / sizeof(PALETTES[0]);
const Palette PALETTE_IDLE = {0x242936, 0x30475E, 0x3A4660, 0x2C3548};

enum Repeat { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE, REPEAT_COUNT };

const char *s_hold_name;    // holder of the card while the app runs (phone_storage_hold)
std::unique_ptr<music::Service> s_lib;  // created in run(), destroyed in close(); the player only runs in between

/* ---- cover ---- */

bool has_ext(const char *path, const char *ext);

constexpr int COVER_ALIGN = 128;            // L2 cache line, required for PPA output buffers
constexpr size_t COVER_FILE_MAX = 3 * 1024 * 1024;
constexpr int COVER_SIDE_MAX = 2048;
enum { COVER_NONE = -1, COVER_ABSENT = 2 }; // s_cover_ready: nothing new, buffer 0 or 1, no cover

uint8_t *s_cover_buf[2];                    // ART x ART RGB565
size_t s_cover_buf_size;
lv_image_dsc_t s_cover_dsc[2];
int s_cover_side[2];
int s_cover_front = -1;                     // buffer on the screen (LVGL task)
std::atomic<int> s_cover_ready{COVER_NONE}; // set by the player task, taken by the LVGL timer

uint32_t be32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

uint32_t syncsafe32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0] & 0x7F) << 21 | static_cast<uint32_t>(p[1] & 0x7F) << 14 |
           static_cast<uint32_t>(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
}

/*
 * Find the attached picture of an ID3v2 tag (APIC, or PIC in v2.2) and return the offset and
 * the size of its JPEG data. Other picture formats (PNG) are not handled.
 */
bool find_cover(FILE *file, long &offset, size_t &size)
{
    uint8_t h[10];
    if (fseek(file, 0, SEEK_SET) != 0 || fread(h, 1, 10, file) != 10 || memcmp(h, "ID3", 3) != 0) {
        return false;
    }
    const int ver = h[3];
    const long tag_end = 10 + static_cast<long>(syncsafe32(h + 6));
    long pos = 10;
    if (h[5] & 0x40) {                      /* extended header */
        uint8_t e[4];
        if (fread(e, 1, 4, file) != 4) {
            return false;
        }
        pos += ver == 4 ? syncsafe32(e) : be32(e) + 4;
    }
    const int head = ver == 2 ? 6 : 10;
    while (pos + head < tag_end) {
        uint8_t f[10];
        if (fseek(file, pos, SEEK_SET) != 0 || fread(f, 1, head, file) != static_cast<size_t>(head) || f[0] == 0) {
            return false;                   /* padding or the end of the tag */
        }
        const uint32_t len = ver == 2 ? (static_cast<uint32_t>(f[3]) << 16 | static_cast<uint32_t>(f[4]) << 8 | f[5])
                             : (ver == 4 ? syncsafe32(f + 4) : be32(f + 4));
        const bool picture = ver == 2 ? memcmp(f, "PIC", 3) == 0 : memcmp(f, "APIC", 4) == 0;
        if (picture && len > 16) {
            /* text encoding, MIME type, picture type and description come first: look for the
             * JPEG start marker instead of parsing them */
            uint8_t probe[512];
            const size_t n = fread(probe, 1, len < sizeof(probe) ? len : sizeof(probe), file);
            for (size_t i = 0; i + 2 < n; i++) {
                if (probe[i] == 0xFF && probe[i + 1] == 0xD8 && probe[i + 2] == 0xFF) {
                    offset = pos + head + static_cast<long>(i);
                    size = len - i;
                    return true;
                }
            }
            return false;
        }
        pos += head + static_cast<long>(len);
    }
    return false;
}

/* Decode the cover of a file into buffer `slot`; false when there is none or it cannot be decoded */
bool load_cover(const std::string &path, int slot)
{
    bool ok = false;
    long offset = 0;
    size_t size = 0;
    size_t in_size = 0, out_size = 0;
    uint8_t *in = nullptr;
    uint8_t *rgb = nullptr;
    jpeg_decoder_handle_t dec = nullptr;
    ppa_client_handle_t ppa = nullptr;
    const jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    const jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    const jpeg_decode_engine_cfg_t eng_cfg = {.intr_priority = 0, .timeout_ms = 2000};
    const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};
    jpeg_decode_picture_info_t info = {};

    FILE *file = has_ext(path.c_str(), ".mp3") ? fopen(path.c_str(), "rb") : nullptr;
    if (!file || !s_cover_buf[slot] || !find_cover(file, offset, size) || size > COVER_FILE_MAX) {
        goto done;
    }
    in = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(size, &in_cfg, &in_size));
    if (!in || fseek(file, offset, SEEK_SET) != 0 || fread(in, 1, size, file) != size ||
            jpeg_decoder_get_info(in, size, &info) != ESP_OK || info.width == 0 || info.height == 0 ||
            info.width > COVER_SIDE_MAX || info.height > COVER_SIDE_MAX) {
        goto done;
    }
    {
        const int w = info.width, h = info.height;
        const int buf_w = (w + 15) & ~15, buf_h = (h + 15) & ~15;
        rgb = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(static_cast<size_t>(buf_w) * buf_h * 2, &out_cfg, &out_size));
        const jpeg_decode_cfg_t cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t len = 0;
        if (!rgb || jpeg_new_decoder_engine(&eng_cfg, &dec) != ESP_OK ||
                jpeg_decoder_process(dec, &cfg, in, size, rgb, out_size, &len) != ESP_OK ||
                ppa_register_client(&ppa_cfg, &ppa) != ESP_OK) {
            ESP_LOGW(TAG, "cover of %s cannot be decoded (%d x %d)", path.c_str(), w, h);
            goto done;
        }
        /* centered square, scaled in 1/16 steps to at most ART pixels */
        const int side = w < h ? w : h;
        int steps = (ART * 16 + side - 1) / side;
        steps = steps < 1 ? 1 : (steps > 64 ? 64 : steps);
        const int block = (side < ART * 16 / steps ? side : ART * 16 / steps) & ~1;
        const int out = block * steps / 16;
        ppa_srm_oper_config_t op = {};
        op.in.buffer = rgb;
        op.in.pic_w = buf_w;
        op.in.pic_h = buf_h;
        op.in.block_w = block;
        op.in.block_h = block;
        op.in.block_offset_x = (w - block) / 2;
        op.in.block_offset_y = (h - block) / 2;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = s_cover_buf[slot];
        op.out.buffer_size = s_cover_buf_size;
        op.out.pic_w = out;
        op.out.pic_h = out;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        op.scale_x = steps / 16.0f;
        op.scale_y = steps / 16.0f;
        op.mode = PPA_TRANS_MODE_BLOCKING;
        ok = out > 0 && out <= ART && ppa_do_scale_rotate_mirror(ppa, &op) == ESP_OK;
        s_cover_side[slot] = out;
    }
done:
    if (ppa) {
        ppa_unregister_client(ppa);
    }
    if (dec) {
        jpeg_del_decoder_engine(dec);
    }
    free(rgb);
    free(in);
    if (file) {
        fclose(file);
    }
    return ok;
}

/* ---- player ---- */

TaskHandle_t s_task;
std::atomic<int> s_req{-1};             // track to start
std::atomic<bool> s_stop;
std::atomic<bool> s_paused;
std::atomic<bool> s_playing;
std::atomic<int> s_cur{-1};
std::atomic<int> s_file_permille;       // position in the file, 0..1000
std::atomic<int> s_elapsed;             // seconds of audio played
std::atomic<bool> s_error;              // the last track could not be played
std::atomic<int> s_repeat{REPEAT_OFF};

bool has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

esp_audio_simple_dec_type_t type_of(const char *path)
{
    static const struct {
        const char *ext;
        esp_audio_simple_dec_type_t type;
    } map[] = {
        {".mp3", ESP_AUDIO_SIMPLE_DEC_TYPE_MP3}, {".wav", ESP_AUDIO_SIMPLE_DEC_TYPE_WAV},
        {".flac", ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC}, {".aac", ESP_AUDIO_SIMPLE_DEC_TYPE_AAC},
        {".m4a", ESP_AUDIO_SIMPLE_DEC_TYPE_M4A}, {".ogg", ESP_AUDIO_SIMPLE_DEC_TYPE_OGG},
    };
    for (const auto &m : map) {
        if (has_ext(path, m.ext)) {
            return m.type;
        }
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

bool interrupted()
{
    return s_stop || s_req >= 0;
}

/* Play one file to its end; returns false when it could not be played */
bool play_file(const std::string &path)
{
    bool ok = false;
    bool opened = false;
    int channels = 1;
    uint32_t rate = 0;
    uint64_t samples = 0;
    int volume = -1;
    size_t pcm_size = PCM_SIZE;
    esp_audio_simple_dec_handle_t dec = nullptr;
    auto *in = static_cast<uint8_t *>(malloc(IN_SIZE));
    auto *pcm = static_cast<uint8_t *>(malloc(pcm_size));
    FILE *file = fopen(path.c_str(), "rb");
    auto spk = static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take());
    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = type_of(path.c_str());
    long size = 0;
    if (file) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fseek(file, 0, SEEK_SET);
    }

    if (!in || !pcm || !file || size <= 0 || !spk || esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "cannot start %s", path.c_str());
        goto done;
    }
    ok = true;
    while (!interrupted()) {
        const size_t n = fread(in, 1, IN_SIZE, file);
        if (n == 0) {
            break;
        }
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = in;
        raw.len = n;
        raw.eos = feof(file) != 0;
        while (raw.len > 0 && !interrupted()) {
            esp_audio_simple_dec_out_t out = {};
            out.buffer = pcm;
            out.len = pcm_size;
            const esp_audio_err_t err = esp_audio_simple_dec_process(dec, &raw, &out);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                auto *bigger = static_cast<uint8_t *>(realloc(pcm, out.needed_size));
                if (!bigger) {
                    ok = false;
                    goto done;
                }
                pcm = bigger;
                pcm_size = out.needed_size;
                continue;
            }
            if (err != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "decode error %d in %s", err, path.c_str());
                ok = opened;        /* a broken tail still counts as played */
                goto done;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (out.decoded_size == 0) {
                continue;
            }
            if (!opened) {
                esp_audio_simple_dec_info_t info = {};
                esp_audio_simple_dec_get_info(dec, &info);
                if (info.bits_per_sample != 16 || info.channel < 1 || info.channel > 2 || info.sample_rate == 0) {
                    ESP_LOGE(TAG, "unsupported format: %u Hz, %u bit, %u ch", static_cast<unsigned>(info.sample_rate),
                             info.bits_per_sample, info.channel);
                    ok = false;
                    goto done;
                }
                channels = info.channel;
                rate = info.sample_rate;
                esp_codec_dev_sample_info_t fs = {};
                fs.sample_rate = rate;
                fs.channel = 1;
                fs.bits_per_sample = 16;
                if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) {
                    ok = false;
                    goto done;
                }
                opened = true;
                ESP_LOGI(TAG, "%s: %u Hz, %d ch", path.c_str(), static_cast<unsigned>(rate), channels);
            }
            /* the speaker is mono: mix stereo down in place */
            auto *s = reinterpret_cast<int16_t *>(pcm);
            size_t frames = out.decoded_size / (2 * channels);
            if (channels == 2) {
                for (size_t i = 0; i < frames; i++) {
                    s[i] = static_cast<int16_t>((s[2 * i] + s[2 * i + 1]) / 2);
                }
            }
            const int v = phone_audio_get_volume();
            if (v != volume) {
                volume = v;
                esp_codec_dev_set_out_vol(spk, v);
            }
            while (s_paused && !interrupted()) {
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            esp_codec_dev_write(spk, s, frames * 2);
            samples += frames;
            s_elapsed = static_cast<int>(samples / rate);
            s_file_permille = static_cast<int>(ftell(file) * 1000LL / size);
        }
    }
done:
    if (opened) {
        esp_codec_dev_close(spk);
    }
    if (spk) {
        phone_audio_speaker_give();
    }
    if (dec) {
        esp_audio_simple_dec_close(dec);
    }
    if (file) {
        fclose(file);
    }
    free(in);
    free(pcm);
    return ok;
}

void player_task(void *)
{
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        int idx = s_req.exchange(-1);
        std::string path;
        while (idx >= 0 && !s_stop && s_lib && s_lib->path(idx, path)) {
            s_cur = idx;
            s_file_permille = 0;
            s_elapsed = 0;
            s_paused = false;
            s_error = false;
            s_playing = true;
            {
                /* into the buffer that is not on the screen */
                const int slot = s_cover_front == 0 ? 1 : 0;
                s_cover_ready = load_cover(path, slot) ? slot : static_cast<int>(COVER_ABSENT);
            }
            const bool ok = play_file(path);
            s_error = !ok;
            const int req = s_req.exchange(-1);
            const int count = s_lib ? static_cast<int>(s_lib->count()) : 0;
            if (req >= 0) {
                idx = req;                          /* another track was chosen */
            } else if (s_stop || !ok) {
                idx = -1;
            } else if (s_repeat == REPEAT_ONE) {
                /* same track again */
            } else if (idx + 1 < count) {
                idx = idx + 1;
            } else {
                idx = s_repeat == REPEAT_ALL ? 0 : -1;  /* end of the list */
            }
        }
        s_playing = false;
        s_stop = false;
    }
}

void play_track(int idx)
{
    if (!s_lib || idx < 0 || idx >= static_cast<int>(s_lib->count())) {
        return;
    }
    if (!s_task && xTaskCreate(player_task, "phone_music", 8192, nullptr, 5, &s_task) != pdPASS) {
        s_task = nullptr;
        return;
    }
    s_stop = false;
    s_req = idx;
    xTaskNotifyGive(s_task);
}

void stop_playback()
{
    if (!s_task) {
        return;
    }
    s_req = -1;
    if (s_playing) {
        s_stop = true;
    }
    for (int i = 0; i < 100 && s_playing; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---- library ---- */

void start_library()
{
    music::Service::Config config;
    config.root = CARD;
    /* tag and duration only for MP3; the other formats keep their file name */
    config.parse = [](const char *path, music::TrackMeta & meta, const music::ParseOptions & options) {
        if (!has_ext(path, ".mp3")) {
            memset(&meta, 0, sizeof(meta));
            return music::ParseResult::IoError;
        }
        return music::parse_mp3_file(path, meta, options);
    };
    config.thread_setup = []() {
        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        cfg.thread_name = "music_meta";
        cfg.stack_size = 6144;
        cfg.prio = 3;
        cfg.inherit_cfg = false;
        esp_pthread_set_cfg(&cfg);
    };
    s_lib = std::make_unique<music::Service>(std::move(config));
    if (phone_storage_get(nullptr, nullptr) == PHONE_STORAGE_MOUNTED) {
        s_lib->start();
    }
}

/* ---- UI ---- */

struct Row {
    lv_obj_t *obj;
    lv_obj_t *number;
    lv_obj_t *title;
    lv_obj_t *artist;
    lv_obj_t *duration;
    music::MetaState shown;
};

lv_obj_t *s_root;
lv_obj_t *s_title;
lv_obj_t *s_artist;
lv_obj_t *s_time;
lv_obj_t *s_duration;
lv_obj_t *s_art;
lv_obj_t *s_art_primary;
lv_obj_t *s_art_secondary;
lv_obj_t *s_cover_img;
lv_obj_t *s_bar;
lv_obj_t *s_play_label;
lv_obj_t *s_repeat_label;
lv_obj_t *s_list;
lv_obj_t *s_empty;
std::vector<Row> s_rows;
bool s_rows_built;
uint32_t s_seen_version;
int s_shown_cur = -2;
int s_shown_state = -1;
uint32_t s_cur_duration_ms;

void format_time(char *buf, size_t len, uint32_t seconds)
{
    snprintf(buf, len, "%u:%02u", static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
}

void on_row(lv_event_t *e)
{
    play_track(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void build_rows()
{
    const int count = static_cast<int>(s_lib->count());
    const int width = lv_obj_get_width(s_list) - 24;
    lv_obj_clean(s_list);
    s_rows.clear();
    for (int i = 0; i < count; i++) {
        Row r = {};
        r.shown = music::MetaState::Pending;
        r.obj = lv_button_create(s_list);
        lv_obj_remove_style_all(r.obj);
        lv_obj_set_size(r.obj, LV_PCT(100), ROW_H);
        lv_obj_set_style_radius(r.obj, 12, 0);
        lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r.obj, lv_color_hex(COLOR_ROW), 0);
        lv_obj_set_style_bg_opa(r.obj, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(r.obj, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_add_event_cb(r.obj, on_row, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        r.number = lv_label_create(r.obj);
        lv_obj_set_style_text_color(r.number, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_font(r.number, phone_font_by_size(16), 0);
        lv_label_set_text_fmt(r.number, "%02d", i + 1);
        lv_obj_align(r.number, LV_ALIGN_LEFT_MID, 14, 0);

        music::TrackView view;
        s_lib->snapshot(i, view);
        r.title = lv_label_create(r.obj);
        lv_label_set_long_mode(r.title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(r.title, width - 54 - 70);
        lv_obj_set_height(r.title, lv_font_get_line_height(phone_font_by_size(20)));
        lv_label_set_text(r.title, view.title);
        lv_obj_align(r.title, LV_ALIGN_LEFT_MID, 54, 0);

        r.artist = lv_label_create(r.obj);
        lv_obj_set_style_text_color(r.artist, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_font(r.artist, phone_font_by_size(14), 0);
        lv_label_set_long_mode(r.artist, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(r.artist, width - 54 - 70);
        lv_obj_set_height(r.artist, lv_font_get_line_height(phone_font_by_size(14)));
        lv_label_set_text(r.artist, "");
        lv_obj_align(r.artist, LV_ALIGN_LEFT_MID, 54, 14);
        lv_obj_add_flag(r.artist, LV_OBJ_FLAG_HIDDEN);

        r.duration = lv_label_create(r.obj);
        lv_obj_set_style_text_color(r.duration, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_font(r.duration, phone_font_by_size(16), 0);
        lv_label_set_text(r.duration, "");
        lv_obj_align(r.duration, LV_ALIGN_RIGHT_MID, -14, 0);
        s_rows.push_back(r);
    }
    if (count == 0) {
        lv_label_set_text(s_empty, T("Chưa có nhạc.\nChép tệp MP3, WAV, FLAC, AAC\nvào thư mục Music trên thẻ nhớ.",
                                     "No music yet.\nCopy MP3, WAV, FLAC or AAC files\nto the Music folder of the card."));
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    s_rows_built = true;
    s_shown_cur = -2;
}

/* Fill in the rows whose tag has been read since the last time */
void refresh_rows()
{
    for (size_t i = 0; i < s_rows.size(); i++) {
        Row &r = s_rows[i];
        if (r.shown != music::MetaState::Pending) {
            continue;
        }
        music::TrackView view;
        if (!s_lib->snapshot(i, view) || view.state == music::MetaState::Pending) {
            continue;
        }
        r.shown = view.state;
        lv_label_set_text(r.title, view.title);
        if (view.artist[0]) {
            lv_label_set_text(r.artist, view.artist);
            lv_obj_remove_flag(r.artist, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(r.title, LV_ALIGN_LEFT_MID, 54, -10);
        }
        if (view.duration_ms) {
            char buf[16];
            format_time(buf, sizeof(buf), view.duration_ms / 1000);
            lv_label_set_text(r.duration, buf);
        }
    }
}

/* Ask for the tags of the rows on the screen first, then of the ones after them */
void update_wanted()
{
    if (s_rows.empty()) {
        return;
    }
    size_t wanted[music::Service::kMaxWanted];
    size_t n = 0;
    const size_t count = s_rows.size();
    const size_t first = static_cast<size_t>(lv_obj_get_scroll_y(s_list) / (ROW_H + ROW_GAP)) % count;
    for (size_t k = 0; k < count && n < music::Service::kMaxWanted; k++) {
        const size_t i = (first + k) % count;
        if (s_rows[i].shown == music::MetaState::Pending) {
            wanted[n++] = i;
        }
    }
    s_lib->set_wanted(wanted, n);
}

void set_artwork(const Palette &p)
{
    lv_obj_set_style_bg_color(s_art, lv_color_hex(p.start), 0);
    lv_obj_set_style_bg_grad_color(s_art, lv_color_hex(p.end), 0);
    lv_obj_set_style_bg_color(s_art_primary, lv_color_hex(p.primary), 0);
    lv_obj_set_style_bg_color(s_art_secondary, lv_color_hex(p.secondary), 0);
}

/* Current row: accent border, a note instead of its number */
void mark_row(int index, bool current)
{
    if (index < 0 || index >= static_cast<int>(s_rows.size())) {
        return;
    }
    Row &r = s_rows[index];
    lv_obj_set_style_bg_color(r.obj, lv_color_hex(current ? COLOR_ROW_CUR : COLOR_ROW), 0);
    lv_obj_set_style_border_width(r.obj, current ? 2 : 0, 0);
    lv_obj_set_style_text_color(r.title, current ? lv_color_hex(COLOR_ACCENT) : lv_color_white(), 0);
    lv_obj_set_style_text_color(r.number, lv_color_hex(current ? COLOR_ACCENT : COLOR_SUB), 0);
    if (current) {
        lv_label_set_text(r.number, LV_SYMBOL_AUDIO);
    } else {
        lv_label_set_text_fmt(r.number, "%02d", index + 1);
    }
}

void show_current(int cur)
{
    mark_row(s_shown_cur, false);
    s_cur_duration_ms = 0;
    if (cur >= 0 && cur < static_cast<int>(s_rows.size())) {
        mark_row(cur, true);
        s_lib->set_pinned(cur);
        set_artwork(PALETTES[cur % PALETTE_COUNT]);
    } else {
        set_artwork(PALETTE_IDLE);
    }
    s_shown_cur = cur;
}

/* Title, artist and duration of the current track; they may arrive after the track started */
void refresh_now_playing(int cur)
{
    music::TrackView view;
    if (cur < 0 || !s_lib->snapshot(cur, view)) {
        return;
    }
    if (strcmp(lv_label_get_text(s_title), view.title) != 0) {
        lv_label_set_text(s_title, view.title);
    }
    if (!s_error && strcmp(lv_label_get_text(s_artist), view.artist) != 0) {
        lv_label_set_text(s_artist, view.artist);
    }
    s_cur_duration_ms = view.duration_ms;
}

void on_timer(lv_timer_t *)
{
    if (!s_root || !s_lib) {
        return;
    }
    if (!s_rows_built && s_lib->scan_done()) {
        build_rows();
    }
    const uint32_t version = s_lib->version();
    if (s_rows_built && version != s_seen_version) {
        s_seen_version = version;
        refresh_rows();
    }
    if (s_rows_built) {
        update_wanted();
    }

    const int cover = s_cover_ready.exchange(COVER_NONE);
    if (cover == 0 || cover == 1) {
        lv_image_dsc_t &dsc = s_cover_dsc[cover];
        dsc = {};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc.header.w = s_cover_side[cover];
        dsc.header.h = s_cover_side[cover];
        dsc.header.stride = s_cover_side[cover] * 2;
        dsc.data_size = s_cover_side[cover] * s_cover_side[cover] * 2;
        dsc.data = s_cover_buf[cover];
        s_cover_front = cover;
        lv_image_cache_drop(&dsc);
        lv_image_set_src(s_cover_img, &dsc);
        lv_obj_center(s_cover_img);
        lv_obj_remove_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    } else if (cover == COVER_ABSENT) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    }

    const int cur = s_cur;
    const bool playing = s_playing;
    const int state = !playing ? 0 : (s_paused ? 1 : 2);
    if (cur != s_shown_cur && s_rows_built) {
        show_current(cur);
    }
    if (state != s_shown_state) {
        s_shown_state = state;
        lv_label_set_text(s_play_label, state == 2 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (cur >= 0) {
        refresh_now_playing(cur);
        const uint32_t t = s_elapsed;
        char buf[16];
        format_time(buf, sizeof(buf), t);
        lv_label_set_text(s_time, buf);
        if (!playing && s_error) {
            lv_label_set_text(s_artist, T("Không phát được tệp này", "Cannot play this file"));
        }
        if (s_cur_duration_ms) {
            format_time(buf, sizeof(buf), s_cur_duration_ms / 1000);
            lv_label_set_text(s_duration, buf);
        } else {
            lv_label_set_text(s_duration, "--:--");
        }
        /* by time when the duration is known, by position in the file otherwise */
        int permille = s_file_permille;
        if (s_cur_duration_ms) {
            permille = static_cast<int>(static_cast<uint64_t>(t) * 1000000ULL / s_cur_duration_ms);
        }
        lv_bar_set_value(s_bar, permille > 1000 ? 1000 : permille, LV_ANIM_OFF);
    }
}

void on_play(lv_event_t *)
{
    if (!s_lib || s_lib->count() == 0) {
        return;
    }
    if (s_playing) {
        s_paused = !s_paused;
    } else {
        play_track(s_cur >= 0 ? static_cast<int>(s_cur) : 0);
    }
}

void on_prev(lv_event_t *)
{
    const int cur = s_cur;
    play_track(cur > 0 ? cur - 1 : 0);
}

void on_next(lv_event_t *)
{
    const int cur = s_cur;
    const int count = s_lib ? static_cast<int>(s_lib->count()) : 0;
    if (cur + 1 < count) {
        play_track(cur + 1);
    } else if (count > 0 && s_repeat == REPEAT_ALL) {
        play_track(0);
    }
}

void show_repeat()
{
    const int mode = s_repeat;
    lv_label_set_text(s_repeat_label, mode == REPEAT_ONE ? LV_SYMBOL_LOOP " 1" : LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(s_repeat_label, lv_color_hex(mode == REPEAT_OFF ? COLOR_SUB : COLOR_ACCENT), 0);
}

/* off, whole list, one track */
void on_repeat(lv_event_t *)
{
    s_repeat = (s_repeat + 1) % REPEAT_COUNT;
    phone_pref_set("mus_rep", s_repeat);
    show_repeat();
}

void on_scroll(lv_event_t *e)
{
    if (s_lib) {
        s_lib->set_ui_idle(lv_event_get_code(e) == LV_EVENT_SCROLL_END);
    }
}

lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, int size, uint32_t color, lv_event_cb_t cb,
                       lv_obj_t **label = nullptr)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    if (label) {
        *label = l;
    }
    return b;
}

void build_ui()
{
    s_shown_cur = -2;
    s_shown_state = -1;
    s_rows.clear();
    s_rows_built = false;
    s_seen_version = 0;
    s_cur_duration_ms = 0;

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

    /* now playing: artwork, title, artist, progress, controls */
    lv_obj_t *card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, root_w - 24, CARD_H - 16);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* artwork placeholder, as in esp_chatbot_v1: a gradient with two discs */
    s_art = lv_obj_create(card);
    lv_obj_remove_style_all(s_art);
    lv_obj_set_size(s_art, ART, ART);
    lv_obj_align(s_art, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_radius(s_art, 22, 0);
    lv_obj_set_style_clip_corner(s_art, true, 0);
    lv_obj_set_style_bg_opa(s_art, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(s_art, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(s_art, LV_OBJ_FLAG_SCROLLABLE);
    s_art_primary = lv_obj_create(s_art);
    lv_obj_remove_style_all(s_art_primary);
    lv_obj_set_size(s_art_primary, ART * 70 / 100, ART * 70 / 100);
    lv_obj_set_pos(s_art_primary, -ART * 17 / 100, ART * 52 / 100);
    lv_obj_set_style_radius(s_art_primary, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_art_primary, LV_OPA_70, 0);
    s_art_secondary = lv_obj_create(s_art);
    lv_obj_remove_style_all(s_art_secondary);
    lv_obj_set_size(s_art_secondary, ART * 56 / 100, ART * 56 / 100);
    lv_obj_set_pos(s_art_secondary, ART * 59 / 100, -ART * 14 / 100);
    lv_obj_set_style_radius(s_art_secondary, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_art_secondary, LV_OPA_60, 0);
    lv_obj_t *note = lv_label_create(s_art);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_opa(note, LV_OPA_80, 0);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_align(note, LV_ALIGN_BOTTOM_RIGHT, -14, -10);
    set_artwork(PALETTE_IDLE);
    /* the cover of the tag, over the placeholder */
    s_cover_img = lv_image_create(s_art);
    lv_obj_center(s_cover_img);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);

    s_title = lv_label_create(card);
    lv_obj_set_style_text_font(s_title, phone_font_by_size(24), 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title, root_w - 72);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, ART + 28);
    lv_label_set_text(s_title, T("Chưa phát", "Not playing"));

    s_artist = lv_label_create(card);
    lv_obj_set_style_text_color(s_artist, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_artist, phone_font_by_size(18), 0);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_artist, root_w - 72);
    lv_obj_set_height(s_artist, lv_font_get_line_height(phone_font_by_size(18)));
    lv_obj_set_style_text_align(s_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_artist, LV_ALIGN_TOP_MID, 0, ART + 62);
    lv_label_set_text(s_artist, "");

    /* elapsed, progress, duration on one line */
    const int line_y = ART + 100;
    s_time = lv_label_create(card);
    lv_obj_set_style_text_color(s_time, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_time, phone_font_by_size(16), 0);
    lv_obj_set_width(s_time, 52);
    lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_LEFT, 8, line_y);
    lv_label_set_text(s_time, "0:00");
    s_duration = lv_label_create(card);
    lv_obj_set_style_text_color(s_duration, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_duration, phone_font_by_size(16), 0);
    lv_obj_set_width(s_duration, 52);
    lv_obj_align(s_duration, LV_ALIGN_TOP_RIGHT, -8, line_y);
    lv_label_set_text(s_duration, "--:--");
    s_bar = lv_bar_create(card);
    lv_obj_set_size(s_bar, root_w - 24 - 2 * 72, 6);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, line_y + 8);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);

    /* repeat, previous, play / pause, next */
    lv_obj_align(round_button(card, LV_SYMBOL_PREV, 60, 0x2A333C, on_prev), LV_ALIGN_BOTTOM_MID, -92, -22);
    lv_obj_align(round_button(card, LV_SYMBOL_PLAY, 76, COLOR_ACCENT, on_play, &s_play_label), LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_align(round_button(card, LV_SYMBOL_NEXT, 60, 0x2A333C, on_next), LV_ALIGN_BOTTOM_MID, 92, -22);

    lv_obj_t *repeat = lv_button_create(card);
    lv_obj_remove_style_all(repeat);
    lv_obj_set_size(repeat, 64, 48);
    lv_obj_set_ext_click_area(repeat, 8);
    lv_obj_align(repeat, LV_ALIGN_BOTTOM_LEFT, 10, -28);
    lv_obj_add_event_cb(repeat, on_repeat, LV_EVENT_CLICKED, nullptr);
    s_repeat_label = lv_label_create(repeat);
    lv_obj_center(s_repeat_label);
    show_repeat();

    /* track list; it ends above the strip where the swipe-up for the navigation bar starts */
    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, root_w, root_h - CARD_H - 36);
    lv_obj_set_y(s_list, CARD_H);
    lv_obj_set_style_pad_hor(s_list, 12, 0);
    lv_obj_set_style_pad_bottom(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, ROW_GAP, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_add_event_cb(s_list, on_scroll, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(s_list, on_scroll, LV_EVENT_SCROLL_END, nullptr);

    s_empty = lv_label_create(root);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, CARD_H + 70);
    lv_label_set_text(s_empty, phone_storage_get(nullptr, nullptr) == PHONE_STORAGE_MOUNTED
                      ? T("Đang tìm nhạc...", "Looking for music...") : T("Không có thẻ nhớ", "No microSD card"));
}

} // namespace

namespace esp_brookesia::apps {

PhoneMusic *PhoneMusic::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneMusic();
    }
    return _instance;
}

PhoneMusic::PhoneMusic():
    App(phone_tr("Nhạc", "Music"), &phone_music_icon, true, true, true)
{
}

PhoneMusic::~PhoneMusic()
{
}

bool PhoneMusic::run()
{
    s_hold_name = phone_tr("Nhạc", "Music");
    phone_storage_hold(s_hold_name);      /* files stay open in the background */
    stop_playback();
    s_cur = -1;
    s_error = false;
    const int repeat = phone_pref_get("mus_rep", REPEAT_OFF);
    s_repeat = repeat >= 0 && repeat < REPEAT_COUNT ? repeat : REPEAT_OFF;
    s_lib.reset();
    start_library();
    s_cover_buf_size = (static_cast<size_t>(ART) * ART * 2 + COVER_ALIGN - 1) & ~static_cast<size_t>(COVER_ALIGN - 1);
    for (auto &buf : s_cover_buf) {
        if (!buf) {
            buf = static_cast<uint8_t *>(heap_caps_aligned_calloc(COVER_ALIGN, 1, s_cover_buf_size, MALLOC_CAP_SPIRAM));
        }
    }
    s_cover_front = -1;
    s_cover_ready = COVER_NONE;
    build_ui();
    lv_timer_create(on_timer, 250, nullptr);
    return true;
}

bool PhoneMusic::back()
{
    return notifyCoreClosed();
}

/* Leaving to the home screen keeps the music playing; closing the app stops it */
bool PhoneMusic::close()
{
    stop_playback();
    s_lib.reset();
    phone_storage_release(s_hold_name);
    /* the image still points at a cover buffer until the core deletes the screen */
    if (s_root && s_cover_img) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_cover_img, nullptr);
    }
    for (auto &buf : s_cover_buf) {
        heap_caps_free(buf);
        buf = nullptr;
    }
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneMusic, APP_NAME, []()
{
    return std::shared_ptr<PhoneMusic>(PhoneMusic::requestInstance(), [](PhoneMusic * p) {});
})

} // namespace esp_brookesia::apps
