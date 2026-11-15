/*
 * Music app: plays the audio files of the microSD card (MP3, WAV, FLAC, AAC, M4A, OGG) on the
 * speaker. Files are taken from /sdcard/Music (with sub-folders) and from the top of the card.
 *
 * A player task reads the file, decodes it with esp_audio_codec and writes mono 16-bit PCM to
 * the speaker codec. It keeps playing while the app is in the background and while the screen
 * is off; closing the app stops it. The volume is the phone volume, followed while playing.
 */
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>
#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"
#include "esp_log.h"
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
constexpr const char *MUSIC_DIR = "/sdcard/Music";
constexpr int MAX_TRACKS = 300;
constexpr size_t IN_SIZE = 4096;
constexpr size_t PCM_SIZE = 16 * 1024;
constexpr int CARD_H = 250;         // now-playing card

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_ROW = 0x1E252C;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0xFF5F78;

/* ---- player ---- */

std::vector<std::string> s_tracks;      // full paths; only changed while the player is idle
TaskHandle_t s_task;
std::atomic<int> s_req{-1};             // track to start
std::atomic<bool> s_stop;
std::atomic<bool> s_paused;
std::atomic<bool> s_playing;
std::atomic<int> s_cur{-1};
std::atomic<int> s_progress;            // 0..1000 of the file
std::atomic<int> s_elapsed;             // seconds of audio played
std::atomic<bool> s_error;              // the last track could not be played

esp_audio_simple_dec_type_t type_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
    static const struct {
        const char *ext;
        esp_audio_simple_dec_type_t type;
    } map[] = {
        {".mp3", ESP_AUDIO_SIMPLE_DEC_TYPE_MP3}, {".wav", ESP_AUDIO_SIMPLE_DEC_TYPE_WAV},
        {".flac", ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC}, {".aac", ESP_AUDIO_SIMPLE_DEC_TYPE_AAC},
        {".m4a", ESP_AUDIO_SIMPLE_DEC_TYPE_M4A}, {".ogg", ESP_AUDIO_SIMPLE_DEC_TYPE_OGG},
    };
    for (const auto &m : map) {
        if (strcasecmp(dot, m.ext) == 0) {
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
            s_progress = static_cast<int>(ftell(file) * 1000LL / size);
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
        while (idx >= 0 && idx < static_cast<int>(s_tracks.size()) && !s_stop) {
            const std::string path = s_tracks[idx];
            s_cur = idx;
            s_progress = 0;
            s_elapsed = 0;
            s_paused = false;
            s_error = false;
            s_playing = true;
            const bool ok = play_file(path);
            s_error = !ok;
            const int req = s_req.exchange(-1);
            if (req >= 0) {
                idx = req;                  /* another track was chosen */
            } else if (!s_stop && ok) {
                idx = idx + 1;              /* next one; stops after the last */
            } else {
                idx = -1;
            }
        }
        s_playing = false;
        s_stop = false;
    }
}

void play_track(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(s_tracks.size())) {
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

void add_dir(const std::string &dir, int depth)
{
    DIR *d = opendir(dir.c_str());
    if (!d) {
        return;
    }
    std::vector<std::string> subdirs;
    while (struct dirent *e = readdir(d)) {
        if (static_cast<int>(s_tracks.size()) >= MAX_TRACKS) {
            break;
        }
        const std::string path = dir + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            if (depth > 0) {
                subdirs.push_back(path);
            }
        } else if (type_of(e->d_name) != ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
            s_tracks.push_back(path);
        }
    }
    closedir(d);
    for (const std::string &s : subdirs) {
        add_dir(s, depth - 1);
    }
}

void scan()
{
    s_tracks.clear();
    if (phone_storage_get(nullptr, nullptr) != PHONE_STORAGE_MOUNTED) {
        return;
    }
    add_dir(MUSIC_DIR, 3);
    add_dir(CARD, 0);
    std::sort(s_tracks.begin(), s_tracks.end(), [](const std::string & a, const std::string & b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
}

/* File name without folder and extension */
std::string title_of(const std::string &path)
{
    const size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

/* ---- UI ---- */

lv_obj_t *s_root;
lv_obj_t *s_title;
lv_obj_t *s_time;
lv_obj_t *s_bar;
lv_obj_t *s_play_label;
std::vector<lv_obj_t *> s_rows;
int s_shown_cur = -2;
int s_shown_state = -1;

void on_play(lv_event_t *)
{
    if (s_tracks.empty()) {
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
    if (!s_tracks.empty()) {
        const int cur = s_cur;
        play_track(cur > 0 ? cur - 1 : 0);
    }
}

void on_next(lv_event_t *)
{
    const int cur = s_cur;
    if (cur + 1 < static_cast<int>(s_tracks.size())) {
        play_track(cur + 1);
    }
}

void on_row(lv_event_t *e)
{
    play_track(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void on_timer(lv_timer_t *)
{
    if (!s_root) {
        return;
    }
    const int cur = s_cur;
    const bool playing = s_playing;
    const int state = !playing ? 0 : (s_paused ? 1 : 2);
    if (cur != s_shown_cur) {
        if (s_shown_cur >= 0 && s_shown_cur < static_cast<int>(s_rows.size())) {
            lv_obj_set_style_bg_color(s_rows[s_shown_cur], lv_color_hex(COLOR_ROW), 0);
        }
        if (cur >= 0 && cur < static_cast<int>(s_rows.size())) {
            lv_obj_set_style_bg_color(s_rows[cur], lv_color_hex(0x4A2630), 0);
            lv_label_set_text(s_title, title_of(s_tracks[cur]).c_str());
        }
        s_shown_cur = cur;
    }
    if (state != s_shown_state) {
        s_shown_state = state;
        lv_label_set_text(s_play_label, state == 2 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (cur >= 0) {
        const int t = s_elapsed;
        if (!playing && s_error) {
            lv_label_set_text(s_time, T("Không phát được tệp này", "Cannot play this file"));
        } else {
            lv_label_set_text_fmt(s_time, "%d:%02d", t / 60, t % 60);
        }
        lv_bar_set_value(s_bar, s_progress, LV_ANIM_OFF);
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

    /* now playing: title, time, progress, controls */
    lv_obj_t *card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, root_w - 24, CARD_H - 16);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(card);
    lv_obj_set_style_text_font(s_title, phone_font_by_size(24), 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title, root_w - 72);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 18);
    lv_label_set_text(s_title, T("Chưa phát", "Not playing"));

    s_time = lv_label_create(card);
    lv_obj_set_style_text_color(s_time, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_time, phone_font_by_size(18), 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 56);
    lv_label_set_text(s_time, "0:00");

    s_bar = lv_bar_create(card);
    lv_obj_set_size(s_bar, root_w - 72, 6);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 92);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);

    lv_obj_align(round_button(card, LV_SYMBOL_PREV, 60, 0x2A333C, on_prev), LV_ALIGN_BOTTOM_MID, -100, -18);
    lv_obj_align(round_button(card, LV_SYMBOL_PLAY, 76, COLOR_ACCENT, on_play, &s_play_label), LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_align(round_button(card, LV_SYMBOL_NEXT, 60, 0x2A333C, on_next), LV_ALIGN_BOTTOM_MID, 100, -18);

    /* track list; it ends above the strip where the swipe-up for the navigation bar starts */
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, root_w, root_h - CARD_H - 36);
    lv_obj_set_y(list, CARD_H);
    lv_obj_set_style_pad_hor(list, 12, 0);
    lv_obj_set_style_pad_bottom(list, 8, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (size_t i = 0; i < s_tracks.size(); i++) {
        lv_obj_t *row = lv_button_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 54);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *icon = lv_label_create(row);
        lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ACCENT), 0);
        lv_label_set_text(icon, LV_SYMBOL_AUDIO);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, root_w - 24 - 64);
        lv_obj_set_height(name, lv_font_get_line_height(phone_font_by_size(20)));
        lv_label_set_text(name, title_of(s_tracks[i]).c_str());
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 50, 0);
        s_rows.push_back(row);
    }
    if (s_tracks.empty()) {
        lv_obj_t *l = lv_label_create(list);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, 60, 0);
        lv_label_set_text(l, phone_storage_get(nullptr, nullptr) == PHONE_STORAGE_MOUNTED
                          ? T("Chưa có nhạc.\nChép tệp MP3, WAV, FLAC, AAC\nvào thư mục Music trên thẻ nhớ.",
                              "No music yet.\nCopy MP3, WAV, FLAC or AAC files\nto the Music folder of the card.")
                          : T("Không có thẻ nhớ", "No microSD card"));
    }
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
    stop_playback();
    s_cur = -1;
    s_error = false;
    scan();
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
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneMusic, APP_NAME, []()
{
    return std::shared_ptr<PhoneMusic>(PhoneMusic::requestInstance(), [](PhoneMusic * p) {});
})

} // namespace esp_brookesia::apps
