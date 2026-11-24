/*
 * Recorder app: records the microphones to WAV files (16 kHz, mono, 16 bit) in /sdcard/Recordings and plays
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

constexpr int RATE = 16000;
constexpr int BYTES_PER_SEC = RATE * 2;
constexpr int CHUNK_SAMPLES = RATE / 20;            // 50 ms: one bar of the wave
constexpr size_t WRITE_SIZE = 32 * 1024;            // bytes gathered before a write to the card
constexpr uint32_t HEADER_EVERY_MS = 5000;          // WAV header written again (sizes so far)
constexpr float MIC_GAIN_DB = 30.0f;
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

/* ---------- LVGL side ---------- */

struct Entry {
    std::string name;       // file name
    uint32_t bytes;         // PCM bytes
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

void put_peak(const int16_t *pcm, int n)
{
    int peak = 0;
    for (int i = 0; i < n; i++) {
        peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
    }
    /* about a log scale: quiet speech still moves the bars */
    int level = 0;
    for (int v = peak; v > 64 && level < 100; v = v * 7 / 8) {
        level += 3;
    }
    const uint32_t i = s_peak_count.load();
    s_peaks[i % BARS] = static_cast<uint8_t>(std::min(level, 100));
    s_peak_count = i + 1;
}

void clear_peaks()
{
    for (auto &p : s_peaks) {
        p = 0;
    }
    s_peak_count = 0;
}

void wav_header(uint8_t *h, uint32_t data_bytes)
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
    put16(h + 22, 1);               // mono
    put32(h + 24, RATE);
    put32(h + 28, BYTES_PER_SEC);
    put16(h + 32, 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}

/* Data bytes of one of our WAV files (44-byte header); from the file size when the header was not
 * written at the end (a reset during the recording) */
uint32_t wav_bytes(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 44) {
        return 0;
    }
    return static_cast<uint32_t>(st.st_size - 44) & ~1u;
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
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = RATE;
    fs.channel = 1;
    fs.bits_per_sample = 16;
    const bool mic_open = mic && esp_codec_dev_open(mic, &fs) == ESP_CODEC_DEV_OK;
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(WRITE_SIZE, MALLOC_CAP_SPIRAM));
    FILE *f = nullptr;
    if (!mic_open) {
        error = REC_ERR_MIC;
    } else {
        esp_codec_dev_set_in_gain(mic, MIC_GAIN_DB);
        f = buf ? fopen(s_rec_path, "wb") : nullptr;
        if (!f) {
            error = REC_ERR_FILE;
        }
    }
    uint8_t header[44];
    uint32_t bytes = 0;
    size_t fill = 0;
    if (f) {
        wav_header(header, 0);
        if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
            error = REC_ERR_WRITE;
        }
        ESP_LOGI(TAG, "recording %s", s_rec_path);
    }
    uint32_t last_header = xTaskGetTickCount();
    int64_t free_left = card_free();
    while (error == REC_OK && !s_rec_stop) {
        /* the buffer is written once the next chunk would not fit; the header follows every few seconds */
        if (WRITE_SIZE - fill < CHUNK_SAMPLES * 2) {
            if (fwrite(buf, 1, fill, f) != fill) {
                error = REC_ERR_WRITE;
                break;
            }
            free_left -= fill;
            fill = 0;
            if (pdTICKS_TO_MS(xTaskGetTickCount() - last_header) >= HEADER_EVERY_MS) {
                last_header = xTaskGetTickCount();
                wav_header(header, bytes);
                fseek(f, 0, SEEK_SET);
                fwrite(header, 1, sizeof(header), f);
                fseek(f, 0, SEEK_END);
                fflush(f);
                fsync(fileno(f));
            }
        }
        if (esp_codec_dev_read(mic, buf + fill, CHUNK_SAMPLES * 2) != ESP_CODEC_DEV_OK) {
            error = REC_ERR_MIC;
            break;
        }
        put_peak(reinterpret_cast<int16_t *>(buf + fill), CHUNK_SAMPLES);
        fill += CHUNK_SAMPLES * 2;
        bytes += CHUNK_SAMPLES * 2;
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
        wav_header(header, bytes);
        fseek(f, 0, SEEK_SET);
        fwrite(header, 1, sizeof(header), f);
        fclose(f);
        ESP_LOGI(TAG, "recorded %lu s, %s", static_cast<unsigned long>(bytes / BYTES_PER_SEC),
                 error == REC_OK ? "ok" : "ended early");
    }
    if (mic_open) {
        esp_codec_dev_close(mic);
    }
    heap_caps_free(buf);
    s_rec_error = error;
    s_recording = false;
    vTaskDelete(nullptr);
}

void play_task(void *)
{
    FILE *f = fopen(s_play_path, "rb");
    auto spk = f ? static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take_yielding([]() {
        s_play_stop = true;     /* music, the radio or a video takes the speaker */
    })) : nullptr;
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = RATE;
    fs.channel = 1;
    fs.bits_per_sample = 16;
    const bool open = spk && esp_codec_dev_open(spk, &fs) == ESP_CODEC_DEV_OK;
    constexpr int N = CHUNK_SAMPLES;
    int16_t pcm[N];
    bool failed = !open;
    if (open) {
        esp_codec_dev_set_out_vol(spk, phone_audio_get_volume());
        fseek(f, 44, SEEK_SET);
        int volume = phone_audio_get_volume();
        while (!s_play_stop) {
            if (s_paused) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            const size_t got = fread(pcm, 2, N, f);
            if (got == 0) {
                break;
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
            s_play_pos = s_play_pos + got * 2;
        }
        esp_codec_dev_close(spk);
    }
    if (spk) {
        phone_audio_speaker_give();
    }
    if (f) {
        fclose(f);
    }
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
        text = mmss(s_play_pos / BYTES_PER_SEC) + " / " + mmss(e.bytes / BYTES_PER_SEC);
    } else {
        text = mmss(e.bytes / BYTES_PER_SEC) + "  ·  " + size_text(e.bytes);
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
                s_entries.push_back({name, wav_bytes(path_of(name).c_str())});
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
    const int64_t secs = std::max<int64_t>(0, free_bytes - MIN_FREE) / BYTES_PER_SEC;
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
        lv_label_set_text(s_time, mmss(s_rec_bytes / BYTES_PER_SEC).c_str());
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
