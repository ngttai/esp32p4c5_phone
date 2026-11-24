/*
 * Recorder app: records the two microphones, mixed, to WAV files (16 kHz, mono, 16 bit) in /sdcard/Recordings and plays
 * them back. Recording goes on behind the lock screen, with the screen off and from the home screen; closing
 * the app ends it and keeps the file. The WAV header is written again every few seconds, so a file cut by a
 * reset or a full card still plays up to there.
 *
 * Two tasks own the audio: the recorder (microphone -> card) and the player (card -> speaker). The LVGL side
 * only sets requests and reads their progress.
 */
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <strings.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "RECORDER"
#include "esp_lib_utils.h"
#include "phone_audio.h"
#include "phone_fonts.h"
#include "phone_led.h"
#include "phone_locale.h"
#include "phone_paths.h"
#include "phone_storage.h"
#include "phone_app_recorder.hpp"

#define APP_NAME "Ghi âm"

extern "C" const lv_image_dsc_t phone_recorder_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

constexpr const char *TAG = "recorder";
constexpr const char *HOLD_NAME = "Recorder";
constexpr const char *REC_DIR = PHONE_SD_RECORDINGS;

/* Settings (NVS, applied to the next recording): channels "rec_ch" (0 mono: both microphones mixed, 1 stereo:
 * MIC1 left, MIC2 right), sample rate "rec_rate" (index in RATES), microphone gain "rec_gain" (index in GAINS) */
constexpr int RATES[] = {16000, 32000, 48000};
constexpr float GAINS[] = {24.0f, 30.0f, 36.0f};
constexpr int CHUNKS_PER_SEC = 20;                  // 50 ms: one bar of the wave
constexpr int MAX_CHUNK = 48000 / CHUNKS_PER_SEC;   // frames of the longest chunk
constexpr size_t WRITE_SIZE = 32 * 1024;            // bytes gathered before a write to the card
constexpr uint32_t HEADER_EVERY_MS = 5000;          // WAV header written again (sizes so far)
constexpr int64_t MIN_FREE = 4 * 1024 * 1024;       // stop before the card is completely full

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_CARD = 0x1E252C;
constexpr uint32_t COLOR_ROW_ON = 0x2A333C;
constexpr uint32_t COLOR_TEXT = 0xFFFFFF;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_REC = 0xEF4444;
constexpr uint32_t COLOR_PLAY = 0x3DDCFF;
constexpr uint32_t COLOR_DANGER = 0xDC2626;

constexpr int BARS = 64;                // bars of the wave
constexpr int BAR_W = 4;
constexpr int BAR_GAP = 3;
constexpr int WAVE_H = 120;
constexpr int BUTTON_D = 96;

/* ---------- shared with the audio tasks ---------- */

std::atomic<bool> s_recording;          // the recorder task runs
std::atomic<bool> s_rec_stop;           // asks it to end
std::atomic<uint32_t> s_rec_bytes;      // PCM bytes written so far
std::atomic<uint32_t> s_rec_byte_rate{32000};   // bytes per second of the recording running
std::atomic<int> s_rec_error;           // 0, or why it ended early (REC_ERR_*)
enum { REC_OK, REC_ERR_MIC, REC_ERR_FILE, REC_ERR_FULL, REC_ERR_WRITE };

std::atomic<bool> s_playing;            // the player task runs
std::atomic<bool> s_play_stop;
std::atomic<bool> s_paused;
std::atomic<uint32_t> s_play_pos;       // PCM bytes played
std::atomic<uint32_t> s_play_total;
std::atomic<bool> s_play_failed;

/* the wave: peaks of the last BARS chunks, 0..100, written by whichever task runs */
std::atomic<uint8_t> s_peaks[BARS];
std::atomic<uint32_t> s_peak_count;

char s_rec_path[96];
char s_play_path[96];
int s_play_rate;                        // format of the file playing (set before the player starts)
int s_play_ch;

int pref_channels()
{
    return phone_pref_get("rec_ch", 0) == 1 ? 2 : 1;
}

int pref_rate()
{
    const int i = phone_pref_get("rec_rate", 0);
    return RATES[i >= 0 && i < 3 ? i : 0];
}

float pref_gain()
{
    const int i = phone_pref_get("rec_gain", 1);
    return GAINS[i >= 0 && i < 3 ? i : 1];
}

/* ---------- LVGL side ---------- */

struct Entry {
    std::string name;       // file name
    uint32_t bytes;         // PCM bytes
    int rate;
    int channels;
    uint32_t byte_rate() const
    {
        return static_cast<uint32_t>(rate) * channels * 2;
    }
};

lv_obj_t *s_root;
lv_obj_t *s_time;
lv_obj_t *s_state;
lv_obj_t *s_wave;
lv_obj_t *s_button;
lv_obj_t *s_button_mark;
lv_obj_t *s_list;
lv_obj_t *s_empty;
std::vector<Entry> s_entries;
std::vector<lv_obj_t *> s_subs;         // second line of each row
std::vector<lv_obj_t *> s_dels;         // delete button of each row
int s_play_index = -1;                  // row playing (or paused)
int s_del_armed = -1;                   // row whose delete asks again
uint32_t s_del_armed_until;
bool s_was_recording;
bool s_was_playing;

void put_level(int peak)
{
    /* about a log scale: quiet speech still moves the bars */
    int level = 0;
    for (int v = peak; v > 64 && level < 100; v = v * 7 / 8) {
        level += 3;
    }
    const uint32_t i = s_peak_count.load();
    s_peaks[i % BARS] = static_cast<uint8_t>(std::min(level, 100));
    s_peak_count = i + 1;
}

void put_peak(const int16_t *pcm, int n)
{
    int peak = 0;
    for (int i = 0; i < n; i++) {
        peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
    }
    put_level(peak);
}

void clear_peaks()
{
    for (auto &p : s_peaks) {
        p = 0;
    }
    s_peak_count = 0;
}

void wav_header(uint8_t *h, uint32_t data_bytes, int rate, int channels)
{
    auto put32 = [](uint8_t *p, uint32_t v) {
        p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
    };
    auto put16 = [](uint8_t *p, uint16_t v) {
        p[0] = v; p[1] = v >> 8;
    };
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);               // PCM
    put16(h + 22, channels);
    put32(h + 24, rate);
    put32(h + 28, rate * channels * 2);
    put16(h + 32, channels * 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}

/* One of our WAV files (44-byte header): rate and channels from the header, data bytes from the file size
 * (also right when the header was not written at the end: a reset during the recording) */
bool wav_info(const char *path, uint32_t *bytes, int *rate, int *channels)
{
    *bytes = 0;
    *rate = 16000;
    *channels = 1;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t h[44];
    const bool ok = fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, "RIFF", 4) == 0;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose(f);
    if (ok) {
        const int ch = h[22] | (h[23] << 8);
        const int r = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
        if ((ch == 1 || ch == 2) && r >= 8000 && r <= 48000) {
            *channels = ch;
            *rate = r;
        }
    }
    if (size > 44) {
        *bytes = static_cast<uint32_t>(size - 44) & ~static_cast<uint32_t>(*channels * 2 - 1);
    }
    return ok;
}

int64_t card_free()
{
    uint64_t total = 0, free_bytes = 0;
    if (phone_storage_get(&total, &free_bytes) != PHONE_STORAGE_MOUNTED) {
        return -1;
    }
    return static_cast<int64_t>(free_bytes);
}

void rec_task(void *)
{
    int error = REC_OK;
    /* the settings as they are now: a change applies to the next recording */
    const int rate = pref_rate();
    const int channels = pref_channels();
    const int chunk = rate / CHUNKS_PER_SEC;
    const size_t chunk_bytes = static_cast<size_t>(chunk) * channels * 2;
    s_rec_byte_rate = static_cast<uint32_t>(rate) * channels * 2;
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = rate;
    fs.channel = BSP_MIC_CHANNELS;          /* frames of BSP_MIC_FRAME_SLOTS int16: MIC1, MIC2 and the reference */
    fs.bits_per_sample = BSP_MIC_BITS;
    const bool mic_open = mic && esp_codec_dev_open(mic, &fs) == ESP_CODEC_DEV_OK;
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(WRITE_SIZE, MALLOC_CAP_SPIRAM));
    auto *frames = static_cast<int16_t *>(heap_caps_malloc(chunk * BSP_MIC_FRAME_SLOTS * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    FILE *f = nullptr;
    if (!mic_open) {
        error = REC_ERR_MIC;
    } else {
        esp_codec_dev_set_in_gain(mic, pref_gain());
        f = buf && frames ? fopen(s_rec_path, "wb") : nullptr;
        if (!f) {
            error = REC_ERR_FILE;
        }
    }
    uint8_t header[44];
    uint32_t bytes = 0;
    size_t fill = 0;
    if (f) {
        wav_header(header, 0, rate, channels);
        if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
            error = REC_ERR_WRITE;
        }
        ESP_LOGI(TAG, "recording %s: %d Hz, %s, %.0f dB", s_rec_path, rate, channels == 2 ? "stereo" : "mono", pref_gain());
    }
    if (error == REC_OK) {
        /* the LED blinks red while recording: seen from the home screen and with the screen off */
        const phone_led_pattern_t blink = {PHONE_LED_BLINK, 0xFF0000, 2000, 120};
        phone_led_request(HOLD_NAME, PHONE_LED_ALERT, &blink);
    }
    uint32_t last_header = xTaskGetTickCount();
    int64_t free_left = card_free();
    while (error == REC_OK && !s_rec_stop) {
        /* the buffer is written once the next chunk would not fit; the header follows every few seconds */
        if (WRITE_SIZE - fill < chunk_bytes) {
            if (fwrite(buf, 1, fill, f) != fill) {
                error = REC_ERR_WRITE;
                break;
            }
            free_left -= fill;
            fill = 0;
            if (pdTICKS_TO_MS(xTaskGetTickCount() - last_header) >= HEADER_EVERY_MS) {
                last_header = xTaskGetTickCount();
                wav_header(header, bytes, rate, channels);
                fseek(f, 0, SEEK_SET);
                fwrite(header, 1, sizeof(header), f);
                fseek(f, 0, SEEK_END);
                fflush(f);
                fsync(fileno(f));
            }
        }
        if (esp_codec_dev_read(mic, frames, chunk * BSP_MIC_FRAME_SLOTS * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            error = REC_ERR_MIC;
            break;
        }
        /* mono: the mean of the two microphones; stereo: MIC1 left, MIC2 right */
        auto *out = reinterpret_cast<int16_t *>(buf + fill);
        int peak = 0;
        for (int i = 0; i < chunk; i++) {
            const int16_t *fr = frames + i * BSP_MIC_FRAME_SLOTS;
            const int m1 = fr[BSP_MIC_SLOT_MIC1], m2 = fr[BSP_MIC_SLOT_MIC2];
            if (channels == 2) {
                out[i * 2] = static_cast<int16_t>(m1);
                out[i * 2 + 1] = static_cast<int16_t>(m2);
            } else {
                out[i] = static_cast<int16_t>((m1 + m2) / 2);
            }
            peak = std::max(peak, std::abs((m1 + m2) / 2));
        }
        put_level(peak);
        fill += chunk_bytes;
        bytes += chunk_bytes;
        s_rec_bytes = bytes;
        if (free_left >= 0 && free_left - static_cast<int64_t>(fill) < MIN_FREE) {
            error = REC_ERR_FULL;
            break;
        }
    }
    if (f) {
        if (fill && fwrite(buf, 1, fill, f) != fill && error == REC_OK) {
            error = REC_ERR_WRITE;
        }
        wav_header(header, bytes, rate, channels);
        fseek(f, 0, SEEK_SET);
        fwrite(header, 1, sizeof(header), f);
        fclose(f);
        ESP_LOGI(TAG, "recorded %lu s, %s", static_cast<unsigned long>(bytes / s_rec_byte_rate),
                 error == REC_OK ? "ok" : "ended early");
    }
    phone_led_release(HOLD_NAME);
    if (mic_open) {
        esp_codec_dev_close(mic);
    }
    heap_caps_free(buf);
    heap_caps_free(frames);
    s_rec_error = error;
    s_recording = false;
    vTaskDelete(nullptr);
}

/* Plays a recording at its own rate; a stereo one is mixed down for the speaker */
void play_task(void *)
{
    const int rate = s_play_rate;
    const int channels = s_play_ch;
    const int chunk = rate / CHUNKS_PER_SEC;
    FILE *f = fopen(s_play_path, "rb");
    auto spk = f ? static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take_yielding([]() {
        s_play_stop = true;     /* music, the radio or a video takes the speaker */
    })) : nullptr;
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = rate;
    fs.channel = 1;
    fs.bits_per_sample = 16;
    const bool open = spk && esp_codec_dev_open(spk, &fs) == ESP_CODEC_DEV_OK;
    auto *pcm = static_cast<int16_t *>(heap_caps_malloc(MAX_CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    bool failed = !open || !pcm;
    if (open && pcm) {
        esp_codec_dev_set_out_vol(spk, phone_audio_get_volume());
        fseek(f, 44, SEEK_SET);
        int volume = phone_audio_get_volume();
        while (!s_play_stop) {
            if (s_paused) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            const size_t got = fread(pcm, 2 * channels, chunk, f);
            if (got == 0) {
                break;
            }
            if (channels == 2) {
                for (size_t i = 0; i < got; i++) {
                    pcm[i] = static_cast<int16_t>((pcm[i * 2] + pcm[i * 2 + 1]) / 2);
                }
            }
            if (phone_audio_get_volume() != volume) {
                volume = phone_audio_get_volume();
                esp_codec_dev_set_out_vol(spk, volume);
            }
            put_peak(pcm, got);
            if (esp_codec_dev_write(spk, pcm, got * 2) != ESP_CODEC_DEV_OK) {
                failed = true;
                break;
            }
            s_play_pos = s_play_pos + got * 2 * channels;
        }
    }
    if (open) {
        esp_codec_dev_close(spk);
    }
    if (spk) {
        phone_audio_speaker_give();
    }
    if (f) {
        fclose(f);
    }
    heap_caps_free(pcm);
    s_play_failed = failed;
    s_playing = false;
    vTaskDelete(nullptr);
}

/* Wait for a task to end after asking it to (the recorder writes the header, the player closes the codec) */
void wait_until_false(std::atomic<bool> &running)
{
    for (int i = 0; i < 300 && running; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void stop_playback()
{
    if (s_playing) {
        s_play_stop = true;
        wait_until_false(s_playing);
    }
}

void stop_recording()
{
    if (s_recording) {
        s_rec_stop = true;
        wait_until_false(s_recording);
    }
}

std::string mmss(uint32_t seconds)
{
    char t[16];
    if (seconds >= 3600) {
        snprintf(t, sizeof(t), "%lu:%02lu:%02lu", static_cast<unsigned long>(seconds / 3600),
                 static_cast<unsigned long>(seconds / 60 % 60), static_cast<unsigned long>(seconds % 60));
    } else {
        snprintf(t, sizeof(t), "%lu:%02lu", static_cast<unsigned long>(seconds / 60), static_cast<unsigned long>(seconds % 60));
    }
    return t;
}

/* "REC_20261124_153012.wav" -> "24/11/2026 15:30"; other names as they are, without ".wav" */
std::string title_of(const std::string &name)
{
    int y, mo, d, h, mi, s;
    if (sscanf(name.c_str(), "REC_%4d%2d%2d_%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6) {
        char t[32];
        snprintf(t, sizeof(t), "%02d/%02d/%04d %02d:%02d", d, mo, y, h, mi);
        return t;
    }
    return name.size() > 4 ? name.substr(0, name.size() - 4) : name;
}

std::string size_text(uint32_t bytes)
{
    char t[24];
    const double mb = (bytes + 44) / (1024.0 * 1024.0);
    snprintf(t, sizeof(t), mb >= 10 ? "%.0f MB" : "%.1f MB", mb);
    return t;
}

std::string path_of(const std::string &name)
{
    return std::string(REC_DIR) + "/" + name;
}

void update_row(int i);
void show_list();

void disarm_delete()
{
    if (s_del_armed >= 0 && s_del_armed < static_cast<int>(s_dels.size())) {
        lv_obj_set_style_bg_color(s_dels[s_del_armed], lv_color_hex(COLOR_CARD), 0);
        lv_label_set_text(lv_obj_get_child(s_dels[s_del_armed], 0), LV_SYMBOL_TRASH);
    }
    s_del_armed = -1;
}

void start_playing(int i)
{
    stop_playback();
    const int old = s_play_index;
    strlcpy(s_play_path, path_of(s_entries[i].name).c_str(), sizeof(s_play_path));
    s_play_index = i;
    s_play_stop = false;
    s_paused = false;
    s_play_failed = false;
    s_play_pos = 0;
    s_play_total = s_entries[i].bytes;
    s_play_rate = s_entries[i].rate;
    s_play_ch = s_entries[i].channels;
    clear_peaks();
    s_playing = true;
    if (xTaskCreatePinnedToCore(play_task, "rec_play", 4096, nullptr, 5, nullptr, 1) != pdPASS) {
        s_playing = false;
    }
    if (old >= 0) {
        update_row(old);
    }
    update_row(i);
}

void on_row(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (i < 0 || i >= static_cast<int>(s_entries.size()) || s_recording) {
        return;
    }
    disarm_delete();
    if (i == s_play_index && s_playing) {
        s_paused = !s_paused;
        update_row(i);
    } else {
        start_playing(i);
    }
}

/* First tap arms the button for 3 s, the second one deletes */
void on_delete(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (i < 0 || i >= static_cast<int>(s_entries.size())) {
        return;
    }
    if (s_del_armed != i || lv_tick_get() >= s_del_armed_until) {
        disarm_delete();
        s_del_armed = i;
        s_del_armed_until = lv_tick_get() + 3000;
        lv_obj_set_style_bg_color(s_dels[i], lv_color_hex(COLOR_DANGER), 0);
        lv_label_set_text(lv_obj_get_child(s_dels[i], 0), T("Xóa?", "Delete?"));
        return;
    }
    s_del_armed = -1;
    if (i == s_play_index) {
        stop_playback();
        s_play_index = -1;
    }
    const std::string path = path_of(s_entries[i].name);
    if (unlink(path.c_str()) != 0) {
        ESP_LOGW(TAG, "cannot delete %s", path.c_str());
    }
    show_list();
}

void update_row(int i)
{
    if (i < 0 || i >= static_cast<int>(s_subs.size())) {
        return;
    }
    const Entry &e = s_entries[i];
    lv_obj_t *row = lv_obj_get_parent(s_subs[i]);
    const bool active = i == s_play_index && s_playing;
    lv_obj_set_style_bg_color(row, lv_color_hex(active ? COLOR_ROW_ON : COLOR_CARD), 0);
    lv_obj_t *icon = lv_obj_get_child(row, 0);
    lv_label_set_text(icon, active && !s_paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(icon, lv_color_hex(active ? COLOR_PLAY : COLOR_SUB), 0);
    std::string text;
    if (active) {
        text = mmss(s_play_pos / e.byte_rate()) + " / " + mmss(e.bytes / e.byte_rate());
    } else {
        text = mmss(e.bytes / e.byte_rate()) + "  ·  " + size_text(e.bytes);
        /* what differs from the default (16 kHz mono) */
        if (e.channels == 2) {
            text += "  ·  Stereo";
        }
        if (e.rate != 16000) {
            text += "  ·  " + std::to_string(e.rate / 1000) + " kHz";
        }
    }
    lv_label_set_text(s_subs[i], text.c_str());
}

void show_list()
{
    s_entries.clear();
    if (DIR *d = opendir(REC_DIR)) {
        while (dirent *de = readdir(d)) {
            const std::string name = de->d_name;
            if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".wav") == 0) {
                Entry e = {name, 0, 16000, 1};
                wav_info(path_of(name).c_str(), &e.bytes, &e.rate, &e.channels);
                s_entries.push_back(e);
            }
        }
        closedir(d);
    }
    /* newest first: the names carry the date */
    std::sort(s_entries.begin(), s_entries.end(), [](const Entry & a, const Entry & b) {
        return a.name > b.name;
    });
    if (s_play_index >= 0) {
        /* the file playing keeps its row */
        int found = -1;
        for (int i = 0; i < static_cast<int>(s_entries.size()); i++) {
            if (path_of(s_entries[i].name) == s_play_path) {
                found = i;
            }
        }
        s_play_index = found;
    }
    lv_obj_clean(s_list);
    s_subs.clear();
    s_dels.clear();
    s_del_armed = -1;
    if (s_entries.empty()) {
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < static_cast<int>(s_entries.size()); i++) {
        void *user = reinterpret_cast<void *>(static_cast<intptr_t>(i));
        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 68);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_CARD), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW_ON), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_SHORT_CLICKED, user);

        lv_obj_t *icon = lv_label_create(row);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, 0);

        lv_obj_t *title = lv_label_create(row);
        lv_label_set_text(title, title_of(s_entries[i].name).c_str());
        lv_obj_set_style_text_font(title, phone_font_by_size(20), 0);
        lv_obj_set_width(title, 300);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 60, 10);

        lv_obj_t *sub = lv_label_create(row);
        lv_obj_set_style_text_font(sub, phone_font_by_size(16), 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_SUB), 0);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 60, -10);
        s_subs.push_back(sub);

        lv_obj_t *del = lv_button_create(row);
        lv_obj_remove_style_all(del);
        lv_obj_set_size(del, 76, 48);
        lv_obj_set_style_radius(del, 12, 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(COLOR_CARD), 0);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_add_event_cb(del, on_delete, LV_EVENT_CLICKED, user);
        lv_obj_t *mark = lv_label_create(del);
        lv_label_set_text(mark, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(mark, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_font(mark, phone_font_by_size(16), 0);
        lv_obj_center(mark);
        s_dels.push_back(del);

        update_row(i);
    }
}

/* A new file name: the date and time when the clock is set, else the next free number */
void new_path()
{
    mkdir(REC_DIR, 0775);
    const time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2025) {
        char name[48];
        strftime(name, sizeof(name), "REC_%Y%m%d_%H%M%S.wav", &tm);
        snprintf(s_rec_path, sizeof(s_rec_path), "%s/%s", REC_DIR, name);
        return;
    }
    for (int n = 1; n < 10000; n++) {
        snprintf(s_rec_path, sizeof(s_rec_path), "%s/REC_%04d.wav", REC_DIR, n);
        struct stat st;
        if (stat(s_rec_path, &st) != 0) {
            return;
        }
    }
}

void show_button()
{
    const bool rec = s_recording;
    lv_obj_set_size(s_button_mark, rec ? 34 : 40, rec ? 34 : 40);
    lv_obj_set_style_radius(s_button_mark, rec ? 6 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_button_mark, lv_color_hex(rec ? 0xFFFFFF : COLOR_REC), 0);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(rec ? COLOR_REC : 0xFFFFFF), 0);
}

void show_idle_state()
{
    const int64_t free_bytes = card_free();
    if (free_bytes < 0) {
        lv_label_set_text(s_state, T("Không có thẻ nhớ", "No memory card"));
        return;
    }
    const int64_t secs = std::max<int64_t>(0, free_bytes - MIN_FREE) / (static_cast<int64_t>(pref_rate()) * pref_channels() * 2);
    char t[96];
    snprintf(t, sizeof(t), T("Ghi được khoảng %lld giờ", "About %lld hours of recording left"),
             static_cast<long long>(secs / 3600));
    lv_label_set_text(s_state, secs >= 3600 ? t : T("Thẻ nhớ sắp đầy", "The memory card is almost full"));
}

void on_record(lv_event_t *)
{
    disarm_delete();
    if (s_recording) {
        stop_recording();
        return;     /* the timer shows the end */
    }
    if (card_free() < MIN_FREE) {
        lv_label_set_text(s_state, card_free() < 0 ? T("Không có thẻ nhớ", "No memory card")
                          : T("Thẻ nhớ đã đầy", "The memory card is full"));
        return;
    }
    stop_playback();
    if (s_play_index >= 0) {
        const int old = s_play_index;
        s_play_index = -1;
        update_row(old);
    }
    new_path();
    s_rec_stop = false;
    s_rec_bytes = 0;
    s_rec_error = REC_OK;
    clear_peaks();
    s_recording = true;
    if (xTaskCreatePinnedToCore(rec_task, "rec_mic", 4096, nullptr, 6, nullptr, 1) != pdPASS) {
        s_recording = false;
        return;
    }
    lv_label_set_text(s_state, T("Đang ghi...", "Recording..."));
    show_button();
}

void on_wave_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(s_wave, &a);
    const int cy = (a.y1 + a.y2) / 2;
    const uint32_t count = s_peak_count;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = BAR_W / 2;
    dsc.bg_color = lv_color_hex(s_recording ? COLOR_REC : s_playing ? COLOR_PLAY : COLOR_SUB);
    const int x0 = a.x1 + (lv_area_get_width(&a) - (BARS * (BAR_W + BAR_GAP) - BAR_GAP)) / 2;
    for (int b = 0; b < BARS; b++) {
        /* the newest peak on the right */
        const int back = BARS - 1 - b;
        int level = 0;
        if (static_cast<uint32_t>(back) < count) {
            level = s_peaks[(count - 1 - back) % BARS];
        }
        const int h = std::max(BAR_W, level * (WAVE_H - 8) / 100);
        lv_area_t r = {x0 + b * (BAR_W + BAR_GAP), cy - h / 2, x0 + b * (BAR_W + BAR_GAP) + BAR_W - 1, cy + h / 2};
        dsc.bg_opa = level ? LV_OPA_COVER : LV_OPA_40;
        lv_draw_rect(layer, &dsc, &r);
    }
}

void on_timer(lv_timer_t *)
{
    if (!s_root) {
        return;
    }
    if (s_del_armed >= 0 && lv_tick_get() >= s_del_armed_until) {
        disarm_delete();
    }
    static uint32_t shown_peaks;
    if (s_peak_count != shown_peaks) {
        shown_peaks = s_peak_count;
        lv_obj_invalidate(s_wave);
    }
    if (s_recording) {
        lv_label_set_text(s_time, mmss(s_rec_bytes / s_rec_byte_rate).c_str());
        s_was_recording = true;
    } else if (s_was_recording) {
        /* the recording just ended: the wave goes flat */
        s_was_recording = false;
        lv_label_set_text(s_time, "0:00");
        clear_peaks();
        lv_obj_invalidate(s_wave);
        show_button();
        switch (s_rec_error.load()) {
        case REC_ERR_MIC:
            lv_label_set_text(s_state, T("Không mở được micro (Trợ lý AI đang dùng?)",
                                         "Could not open the microphone (AI Assistant using it?)"));
            break;
        case REC_ERR_FILE:
        case REC_ERR_WRITE:
            lv_label_set_text(s_state, T("Không ghi được vào thẻ nhớ", "Could not write to the memory card"));
            break;
        case REC_ERR_FULL:
            lv_label_set_text(s_state, T("Thẻ nhớ đã đầy: đã lưu bản ghi", "The memory card is full: recording saved"));
            break;
        default:
            show_idle_state();
            break;
        }
        show_list();
    }
    if (s_play_index >= 0) {
        if (s_playing) {
            update_row(s_play_index);
            s_was_playing = true;
        } else if (s_was_playing) {
            s_was_playing = false;
            clear_peaks();
            lv_obj_invalidate(s_wave);
            const int i = s_play_index;
            s_play_index = -1;
            update_row(i);
            if (s_play_failed) {
                lv_label_set_text(s_state, T("Loa đang được dùng", "The speaker is in use"));
            }
        }
    }
}

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* ---------- settings sheet ---------- */

struct Option {
    const char *key;
    int def;
    const char *title_vi, *title_en;
    const char *names[3];
    int count;
};

const Option OPTIONS[] = {
    {"rec_ch", 0, "Kênh", "Channels", {"Mono", "Stereo", nullptr}, 2},
    {"rec_rate", 0, "Chất lượng", "Quality", {"16 kHz", "32 kHz", "48 kHz"}, 3},
    {"rec_gain", 1, "Độ nhạy micro", "Microphone gain", {nullptr, nullptr, nullptr}, 3},
};
constexpr int OPTION_COUNT = sizeof(OPTIONS) / sizeof(OPTIONS[0]);

lv_obj_t *s_sheet;
lv_obj_t *s_opt_btns[OPTION_COUNT][3];

const char *option_name(int o, int v)
{
    if (o == 2) {
        static const char *const vi[] = {"Thấp", "Vừa", "Cao"};
        static const char *const en[] = {"Low", "Medium", "High"};
        return T(vi[v], en[v]);
    }
    return OPTIONS[o].names[v];
}

void show_options()
{
    for (int o = 0; o < OPTION_COUNT; o++) {
        const int cur = phone_pref_get(OPTIONS[o].key, OPTIONS[o].def);
        for (int v = 0; v < OPTIONS[o].count; v++) {
            lv_obj_set_style_bg_color(s_opt_btns[o][v], lv_color_hex(v == cur ? 0x2F80ED : COLOR_ROW_ON), 0);
        }
    }
}

void on_option(lv_event_t *e)
{
    const int code = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const int o = code / 16, v = code % 16;
    phone_pref_set(OPTIONS[o].key, v);
    show_options();
    if (!s_recording) {
        show_idle_state();      /* the time left depends on the format */
    }
}

void build_sheet(lv_obj_t *root)
{
    /* a shade over the app; a tap on it closes the sheet */
    s_sheet = lv_obj_create(root);
    lv_obj_remove_style_all(s_sheet);
    lv_obj_add_flag(s_sheet, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_size(s_sheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_sheet, lv_color_hex(0x000000), 0);
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sheet, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == s_sheet) {
            lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *card = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);       /* a tap on the card does not close the sheet */
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, T("Cài đặt ghi âm", "Recording settings"));
    lv_obj_set_style_text_font(title, phone_font_by_size(24), 0);

    for (int o = 0; o < OPTION_COUNT; o++) {
        lv_obj_t *cap = lv_label_create(card);
        lv_label_set_text(cap, T(OPTIONS[o].title_vi, OPTIONS[o].title_en));
        lv_obj_set_style_text_color(cap, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_font(cap, phone_font_by_size(18), 0);
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 52);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        for (int v = 0; v < OPTIONS[o].count; v++) {
            lv_obj_t *b = lv_button_create(row);
            lv_obj_remove_style_all(b);
            lv_obj_set_height(b, LV_PCT(100));
            lv_obj_set_flex_grow(b, 1);
            lv_obj_set_style_radius(b, 12, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_add_event_cb(b, on_option, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(o * 16 + v)));
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, option_name(o, v));
            lv_obj_set_style_text_font(l, phone_font_by_size(18), 0);
            lv_obj_center(l);
            s_opt_btns[o][v] = b;
        }
    }
    lv_obj_t *note = lv_label_create(card);
    lv_label_set_text(note, T("Áp dụng từ lần ghi sau. Stereo: micro 1 bên trái, micro 2 bên phải.",
                              "Applies to the next recording. Stereo: microphone 1 left, microphone 2 right."));
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(note, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(note, phone_font_by_size(16), 0);
    show_options();

    /* the gear, top right of the app */
    lv_obj_t *gear = lv_button_create(root);
    lv_obj_remove_style_all(gear);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(gear, 56, 56);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, 0, -6);
    lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(gear, lv_color_hex(COLOR_CARD), LV_STATE_PRESSED);
    lv_obj_add_event_cb(gear, [](lv_event_t *) {
        show_options();
        lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_sheet);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *gl = lv_label_create(gear);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gl, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(gl, &lv_font_montserrat_24, 0);
    lv_obj_center(gl);
}

void build_ui()
{
    lv_obj_t *root = plain(lv_screen_active());
    s_root = root;
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(root, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(20), 0);
    lv_obj_set_style_pad_hor(root, 16, 0);
    lv_obj_set_style_pad_top(root, 16, 0);
    lv_obj_set_style_pad_bottom(root, 40, 0);       /* the list ends above the navigation strip */
    lv_obj_set_style_pad_row(root, 12, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    s_time = lv_label_create(root);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_44, 0);
    lv_label_set_text(s_time, "0:00");

    s_state = lv_label_create(root);
    lv_obj_set_style_text_color(s_state, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_state, phone_font_by_size(16), 0);
    lv_obj_set_width(s_state, LV_PCT(100));
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_state, LV_LABEL_LONG_MODE_WRAP);

    s_wave = plain(root);
    lv_obj_set_size(s_wave, LV_PCT(100), WAVE_H);
    lv_obj_add_event_cb(s_wave, on_wave_draw, LV_EVENT_DRAW_MAIN, nullptr);

    s_button = lv_button_create(root);
    lv_obj_remove_style_all(s_button);
    lv_obj_set_size(s_button, BUTTON_D, BUTTON_D);
    lv_obj_set_style_radius(s_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_button, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_button, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_button, on_record, LV_EVENT_CLICKED, nullptr);
    s_button_mark = plain(s_button);
    lv_obj_set_style_bg_opa(s_button_mark, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_button_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_button_mark);

    lv_obj_t *caption = lv_label_create(root);
    lv_label_set_text(caption, T("Bản ghi", "Recordings"));
    lv_obj_set_style_text_color(caption, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(caption, phone_font_by_size(24), 0);
    lv_obj_set_width(caption, LV_PCT(100));

    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    /* a thin bar while scrolling, as in Settings, in a gap right of the rows */
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_pad_right(s_list, 10, 0);
    lv_obj_set_style_width(s_list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(COLOR_TEXT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(s_list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(s_list, 4, LV_PART_SCROLLBAR);

    s_empty = lv_label_create(root);
    lv_label_set_text(s_empty, T("Chưa có bản ghi. Chạm nút đỏ để ghi.", "No recordings yet. Tap the red button to record."));
    lv_obj_set_style_text_color(s_empty, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_empty, phone_font_by_size(16), 0);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(s_empty, LV_ALIGN_BOTTOM_MID, 0, -120);

    /* the list takes the height left above the navigation strip (a fixed height: it scrolls inside it) */
    lv_obj_update_layout(root);
    lv_area_t rc, lc;
    lv_obj_get_coords(root, &rc);
    lv_obj_get_coords(s_list, &lc);
    const int list_h = rc.y2 - lv_obj_get_style_pad_bottom(root, LV_PART_MAIN) - lc.y1;
    lv_obj_set_flex_grow(s_list, 0);
    lv_obj_set_height(s_list, std::max(list_h, 100));
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    ESP_LOGI(TAG, "list height %d", list_h);

    build_sheet(root);
    show_button();
    if (s_recording) {
        lv_label_set_text(s_state, T("Đang ghi...", "Recording..."));
        s_was_recording = true;
    } else {
        show_idle_state();
    }
    show_list();
}

} // namespace

namespace esp_brookesia::apps {

PhoneRecorder *PhoneRecorder::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneRecorder();
    }
    return _instance;
}

PhoneRecorder::PhoneRecorder():
    App(phone_tr("Ghi âm", "Recorder"), &phone_recorder_icon, true, true, phone_nav_bar())
{
}

PhoneRecorder::~PhoneRecorder()
{
}

bool PhoneRecorder::run()
{
    phone_storage_hold(HOLD_NAME);        /* the file being written stays open in the background */
    s_play_index = -1;
    s_del_armed = -1;
    s_was_playing = false;
    clear_peaks();
    build_ui();
    lv_timer_create(on_timer, 50, nullptr);
    return true;
}

bool PhoneRecorder::back()
{
    return notifyCoreClosed();
}

/* From the home screen a recording or a playback goes on; closing the app ends them (the file is kept) */
bool PhoneRecorder::keepRunning() const
{
    return s_recording || s_playing;
}

bool PhoneRecorder::close()
{
    stop_recording();
    stop_playback();
    s_was_recording = false;
    s_play_index = -1;
    s_entries.clear();
    s_subs.clear();
    s_dels.clear();
    phone_storage_release(HOLD_NAME);
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneRecorder, APP_NAME, []()
{
    return std::shared_ptr<PhoneRecorder>(PhoneRecorder::requestInstance(), [](PhoneRecorder * p) {});
})

} // namespace esp_brookesia::apps
