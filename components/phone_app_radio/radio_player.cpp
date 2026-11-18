/*
 * Radio player: one play task runs esp_audio_simple_player_run_to_end() on the station URL and retries;
 * stop requests interrupt it with esp_audio_simple_player_stop() from a short worker task, so that the
 * LVGL task never waits on the pipeline. The output callback (in the player's own task) opens the codec
 * at the stream rate, mixes to mono, writes to the speaker and feeds the spectrum task.
 */
#include "radio_player.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <strings.h>
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_fourcc.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_fft.h"
#include "esp_gmf_fft_heap.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_heap_caps.h"
#include "esp_hls_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include "phone_audio.h"

namespace radio {

namespace {

const char *TAG = "radio";
constexpr int MAX_CONNECT_FAILURES = 3;     // attempts in a row without sound before giving up
constexpr uint32_t RETRY_MS = 1200;
constexpr int FFT_N = 256;
constexpr size_t RING = 2048;               // mono samples for the spectrum, power of 2
constexpr float ATTACK = 0.55f;
constexpr float RELEASE = 0.18f;

std::mutex s_mtx;                           // s_url
std::string s_url;
std::atomic<uint32_t> s_gen;                // station changes
std::atomic<bool> s_want;                   // the user wants sound
std::atomic<bool> s_quit;
std::atomic<bool> s_in_run;                 // inside run_to_end
std::atomic<bool> s_stop_busy;              // a stop worker is running
std::atomic<bool> s_heard;                  // sound came out in this run
std::atomic<int> s_status{static_cast<int>(Status::STOPPED)};
std::atomic<bool> s_speaker_busy;
std::atomic<TaskHandle_t> s_task;
std::atomic<TaskHandle_t> s_fft_task;
esp_asp_handle_t s_player;                  // play task only (and the stop worker while s_stop_busy)

/* stream format, from the MUSIC_INFO event and the HLS segment callback */
std::atomic<int> s_rate;
std::atomic<int> s_channels{2};
std::atomic<uint32_t> s_seg_format;
uint32_t s_hls_last_format;

/* output, in the player task */
esp_codec_dev_handle_t s_spk;               // taken by the play task
int s_open_rate;
int s_volume = -1;
int16_t *s_mono;
size_t s_mono_cap;

/* spectrum */
alignas(16) int16_t s_ring[RING];
std::atomic<uint32_t> s_ring_w;
std::atomic<uint32_t> s_ring_r;
portMUX_TYPE s_bar_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_bars[BAR_COUNT];

void set_status(Status st)
{
    s_status = static_cast<int>(st);
}

/* ---- spectrum ---- */

void ring_push(const int16_t *p, int n)
{
    uint32_t w = s_ring_w.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        s_ring[w++ & (RING - 1)] = p[i];
    }
    s_ring_w.store(w, std::memory_order_release);
    const uint32_t r = s_ring_r.load(std::memory_order_relaxed);
    if (w - r > RING) {
        s_ring_r.store(w - RING, std::memory_order_relaxed);
    }
}

bool ring_pop(int16_t *out, int n)
{
    const uint32_t w = s_ring_w.load(std::memory_order_acquire);
    uint32_t r = s_ring_r.load(std::memory_order_relaxed);
    if (w - r < static_cast<uint32_t>(n)) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        out[i] = s_ring[r++ & (RING - 1)];
    }
    s_ring_r.store(r, std::memory_order_release);
    return true;
}

void publish(const float *levels)
{
    portENTER_CRITICAL(&s_bar_mux);
    for (int i = 0; i < BAR_COUNT; i++) {
        s_bars[i] = static_cast<uint8_t>(std::clamp(levels[i], 0.f, 1.f) * 255.f + 0.5f);
    }
    portEXIT_CRITICAL(&s_bar_mux);
}

uint32_t isqrt(uint32_t n)
{
    if (n == 0) {
        return 0;
    }
    uint32_t x = n, y = (n + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return x;
}

uint32_t magnitude(const int16_t *d, int k)
{
    int32_t re, im = 0;
    if (k == 0) {
        re = d[0];
    } else if (k == FFT_N / 2) {
        re = d[1];
    } else {
        re = d[2 * k];
        im = d[2 * k + 1];
    }
    return isqrt(static_cast<uint32_t>(re * re) + static_cast<uint32_t>(im * im));
}

/* 256-point FFT of the mono sound, 12 log bands 60 Hz - 6 kHz at the stream rate, about 30 times a second */
void fft_task(void *)
{
    esp_gmf_fft_handle_t fft = nullptr;
    const esp_gmf_fft_cfg_t cfg = {.n_fft = FFT_N, .fft_type = ESP_GMF_FFT_TYPE_REAL_Q15};
    auto *buf = static_cast<int16_t *>(esp_gmf_fft_calloc_aligned(ESP_GMF_FFT_BUFFER_SIZE(FFT_N), sizeof(int16_t), 16));
    if (!buf || esp_gmf_fft_init(&cfg, &fft) != ESP_GMF_FFT_OK) {
        ESP_LOGE(TAG, "fft init failed");
        esp_gmf_fft_free_aligned(buf);
        s_fft_task = nullptr;
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    static int16_t hann[FFT_N];
    for (int i = 0; i < FFT_N; i++) {
        hann[i] = static_cast<int16_t>(0.5f * (1.f - cosf(2.f * static_cast<float>(M_PI) * i / (FFT_N - 1))) * 32767.f);
    }
    int lo[BAR_COUNT], hi[BAR_COUNT];
    int bands_rate = 0;
    float smooth[BAR_COUNT] = {};
    static int16_t pcm[FFT_N];
    while (!s_quit) {
        const int rate = s_rate;
        if (!s_want || rate <= 0) {
            for (float &v : smooth) {
                v *= 1.f - RELEASE;
            }
            publish(smooth);
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (rate != bands_rate) {
            bands_rate = rate;
            const float bin_hz = static_cast<float>(rate) / FFT_N;
            for (int b = 0; b < BAR_COUNT; b++) {
                const float f0 = 60.f * powf(100.f, static_cast<float>(b) / BAR_COUNT);
                const float f1 = 60.f * powf(100.f, static_cast<float>(b + 1) / BAR_COUNT);
                lo[b] = std::max(1, static_cast<int>(f0 / bin_hz));
                hi[b] = std::max(lo[b], std::min(FFT_N / 2 - 1, static_cast<int>(f1 / bin_hz)));
            }
        }
        if (!ring_pop(pcm, FFT_N)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        for (int i = 0; i < FFT_N; i++) {
            buf[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[i]) * hann[i]) >> 15);
        }
        for (size_t i = FFT_N; i < ESP_GMF_FFT_BUFFER_SIZE(FFT_N); i++) {
            buf[i] = 0;
        }
        if (esp_gmf_fft_forward(fft, buf) != ESP_GMF_FFT_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        float raw[BAR_COUNT];
        float peak = 1.f;
        for (int b = 0; b < BAR_COUNT; b++) {
            uint32_t sum = 0;
            for (int k = lo[b]; k <= hi[b]; k++) {
                sum += magnitude(buf, k);
            }
            raw[b] = static_cast<float>(sum) / (hi[b] - lo[b] + 1);
            peak = std::max(peak, raw[b]);
        }
        for (int b = 0; b < BAR_COUNT; b++) {
            const float n = sqrtf(std::clamp(raw[b] / peak, 0.f, 1.f));
            smooth[b] += (n - smooth[b]) * (n > smooth[b] ? ATTACK : RELEASE);
        }
        publish(smooth);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    const float zero[BAR_COUNT] = {};
    publish(zero);
    esp_gmf_fft_free_aligned(buf);
    esp_gmf_fft_deinit(&fft);
    s_fft_task = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

/* ---- player ---- */

void close_codec()
{
    if (s_spk && s_open_rate) {
        esp_codec_dev_close(s_spk);
    }
    s_open_rate = 0;
}

int on_output(uint8_t *data, int size, void *)
{
    if (!s_want || s_quit || !s_spk || size <= 0) {
        return 0;
    }
    const int rate = s_rate;
    const int ch = std::max(1, s_channels.load());
    if (rate <= 0) {
        return 0;
    }
    if (rate != s_open_rate) {
        close_codec();
        esp_codec_dev_sample_info_t fs = {};
        fs.sample_rate = rate;
        fs.channel = 1;
        fs.bits_per_sample = 16;
        if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec open %d Hz failed", rate);
            return 0;
        }
        s_open_rate = rate;
        s_volume = -1;
    }
    const int frames = size / 2 / ch;
    if (static_cast<size_t>(frames) > s_mono_cap) {
        heap_caps_free(s_mono);
        s_mono = static_cast<int16_t *>(heap_caps_malloc(frames * 2, MALLOC_CAP_SPIRAM));
        s_mono_cap = s_mono ? frames : 0;
        if (!s_mono) {
            return 0;
        }
    }
    const auto *pcm = reinterpret_cast<const int16_t *>(data);
    for (int i = 0; i < frames; i++) {
        int sum = 0;
        for (int c = 0; c < ch; c++) {
            sum += pcm[i * ch + c];
        }
        s_mono[i] = static_cast<int16_t>(sum / ch);
    }
    const int v = phone_audio_get_volume();
    if (v != s_volume) {
        s_volume = v;
        esp_codec_dev_set_out_vol(s_spk, v);
    }
    esp_codec_dev_write(s_spk, s_mono, frames * 2);
    ring_push(s_mono, frames);
    if (!s_heard.exchange(true)) {
        set_status(Status::PLAYING);
    }
    return 0;
}

int on_event(esp_asp_event_pkt_t *ev, void *)
{
    if (ev && ev->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO && ev->payload_size >= static_cast<int>(sizeof(esp_asp_music_info_t))) {
        esp_asp_music_info_t info = {};
        memcpy(&info, ev->payload, sizeof(info));
        if (info.channels > 0) {
            s_channels = info.channels;
        }
        if (info.sample_rate > 0) {
            s_rate = info.sample_rate;
        }
        ESP_LOGI(TAG, "stream: %d Hz, %d ch, %d bit", info.sample_rate, info.channels, info.bits);
    }
    return 0;
}

/* Each HLS segment: the decoder follows its format (AAC, MP3, TS) */
int on_segment(esp_hls_file_seg_info_t *info, void *ctx)
{
    if (!info || !ctx || s_hls_last_format == info->format) {
        return 0;
    }
    s_hls_last_format = info->format;
    s_seg_format = info->format;
    esp_gmf_pipeline_handle_t pipe = nullptr;
    esp_gmf_element_handle_t dec = nullptr;
    if (esp_audio_simple_player_get_pipeline(static_cast<esp_asp_handle_t>(ctx), &pipe) != ESP_GMF_ERR_OK || !pipe ||
            esp_gmf_pipeline_get_el_by_name(pipe, "aud_dec", &dec) != ESP_GMF_ERR_OK || !dec) {
        return 0;
    }
    esp_gmf_info_sound_t snd = {};
    snd.format_id = info->format;
    snd.sample_rates = 44100;
    snd.channels = 2;
    snd.bits = 16;
    esp_gmf_audio_dec_reconfig_by_sound_info(dec, &snd);
    ESP_LOGI(TAG, "HLS segments: %s", ESP_FOURCC_TO_STR(info->format));
    return 0;
}

/* The pool has a plain "io_http" already; HLS fetches over https need the certificate bundle, so this
 * one has its own tag and scores higher on http(s) URLs that are not playlists */
esp_gmf_err_t http_score(esp_gmf_io_handle_t, const char *url, int *score)
{
    *score = ESP_GMF_IO_SCORE_NONE;
    if (!url) {
        return ESP_GMF_ERR_OK;
    }
    const char *file = strrchr(url, '/');
    file = file ? file + 1 : url;
    if (strstr(file, ".m3u8") || strstr(file, ".M3U8")) {
        return ESP_GMF_ERR_OK;
    }
    if (strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0) {
        *score = ESP_GMF_IO_SCORE_STANDARD + 10;
    }
    return ESP_GMF_ERR_OK;
}

void destroy_player()
{
    if (s_player) {
        esp_audio_simple_player_stop(s_player);
        esp_audio_simple_player_destroy(s_player);
        s_player = nullptr;
    }
}

bool create_player()
{
    static bool s_media_lib;
    if (!s_media_lib) {
        media_lib_add_default_adapter();
        s_media_lib = true;
    }
    /* GMF logs several lines per segment */
    for (const char *tag : {"ESP_GMF_TASK", "ESP_GMF_BLOCK", "ESP_GMF_PORT", "NEW_DATA_BUS", "ESP_GMF_POOL", "ESP_GMF_EL",
                            "ESP_ES_PARSER", "AUD_SIMP_PLAYER", "HLS_Playlist", "esp-x509-crt-bundle", "ESP_GMF_ADEC",
                            "ESP_GMF_HTTP", "HTTP_CLIENT"
                           }) {
        esp_log_level_set(tag, ESP_LOG_WARN);
    }
    esp_asp_cfg_t cfg = {};
    cfg.out.cb = on_output;
    cfg.task_prio = 5;
    cfg.task_stack = 8 * 1024;
    cfg.task_core = 1;                      /* away from the LVGL task */
    cfg.task_stack_in_ext = true;
    if (esp_audio_simple_player_new(&cfg, &s_player) != ESP_GMF_ERR_OK || !s_player) {
        s_player = nullptr;
        return false;
    }
    esp_gmf_pool_handle_t pool = nullptr;
    if (esp_audio_simple_player_get_pool(s_player, &pool) != ESP_GMF_ERR_OK || !pool) {
        destroy_player();
        return false;
    }
    http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.dir = ESP_GMF_IO_DIR_READER;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_gmf_io_handle_t http_io = nullptr;
    if (esp_gmf_io_http_init(&http_cfg, &http_io) == ESP_GMF_ERR_OK && http_io) {
        reinterpret_cast<esp_gmf_io_t *>(http_io)->get_score = http_score;
        esp_gmf_obj_set_tag(reinterpret_cast<esp_gmf_obj_handle_t>(http_io), "io_http_tls");
        if (esp_audio_simple_player_register_io(s_player, http_io) != ESP_GMF_ERR_OK) {
            esp_gmf_obj_delete(http_io);
        }
    }
    esp_hls_io_cfg_t hls_cfg = {};
    hls_cfg.name = "io_hls";
    hls_cfg.file_seg_cb = on_segment;
    hls_cfg.ctx = s_player;
    hls_cfg.pool = pool;
    hls_cfg.io_cfg = DEFAULT_HLS_IO_CFG();
    esp_gmf_io_handle_t hls_io = nullptr;
    if (esp_gmf_io_hls_init(&hls_cfg, &hls_io) != ESP_GMF_ERR_OK || !hls_io) {
        destroy_player();
        return false;
    }
    if (esp_audio_simple_player_register_io(s_player, hls_io) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(hls_io);
        destroy_player();
        return false;
    }
    esp_audio_simple_player_set_event(s_player, on_event, nullptr);
    return true;
}

void release_speaker()
{
    close_codec();
    if (s_spk) {
        s_spk = nullptr;
        phone_audio_speaker_give();
    }
}

/* Interrupts run_to_end() without making the caller wait for the pipeline */
void interrupt()
{
    if (!s_in_run || s_stop_busy.exchange(true)) {
        return;
    }
    auto worker = [](void *) {
        if (s_player) {
            esp_audio_simple_player_stop(s_player);
        }
        s_stop_busy = false;
        vTaskDeleteWithCaps(nullptr);
    };
    if (xTaskCreatePinnedToCoreWithCaps(worker, "radio_stop", 4096, nullptr, 6, nullptr, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_stop_busy = false;
    }
}

void play_task(void *)
{
    int failures = 0;
    uint32_t failure_gen = s_gen;
    while (!s_quit) {
        if (!s_want) {
            release_speaker();
            if (s_status != static_cast<int>(Status::FAILED)) {
                set_status(Status::STOPPED);
            }
            failures = 0;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }
        if (!s_spk) {
            s_spk = static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take());
            if (!s_spk) {
                ESP_LOGW(TAG, "speaker busy");
                s_speaker_busy = true;
                set_status(Status::FAILED);
                s_want = false;
                continue;
            }
        }
        if (!s_player && !create_player()) {
            ESP_LOGE(TAG, "cannot create the player");
            set_status(Status::FAILED);
            s_want = false;
            continue;
        }
        const uint32_t gen = s_gen;
        if (gen != failure_gen) {
            failure_gen = gen;
            failures = 0;
        }
        std::string url;
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            url = s_url;
        }
        s_heard = false;
        s_hls_last_format = 0;
        s_speaker_busy = false;
        set_status(failures ? Status::RETRYING : Status::CONNECTING);
        ESP_LOGI(TAG, "play %s", url.c_str());
        s_in_run = true;
        if (s_quit || !s_want || s_gen != gen) {
            s_in_run = false;               /* changed while the URL was read */
            continue;
        }
        const esp_gmf_err_t err = esp_audio_simple_player_run_to_end(s_player, url.c_str(), nullptr);
        s_in_run = false;
        while (s_stop_busy) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        close_codec();
        if (s_quit || !s_want || s_gen != gen) {
            continue;                       /* stopped, or another station */
        }
        if (err != ESP_GMF_ERR_OK || !s_heard) {
            failures = s_heard ? 0 : failures + 1;   /* a drop while playing is not a failure to connect */
            ESP_LOGW(TAG, "stream ended (0x%x), %d attempts without sound", err, failures);
            if (failures >= MAX_CONNECT_FAILURES) {
                set_status(Status::FAILED);
                s_want = false;
                continue;
            }
            set_status(Status::RETRYING);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));  /* the stream ended cleanly: open it again */
        }
    }
    destroy_player();
    release_speaker();
    heap_caps_free(s_mono);
    s_mono = nullptr;
    s_mono_cap = 0;
    set_status(Status::STOPPED);
    s_task = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

void ensure_tasks()
{
    s_quit = false;
    if (!s_task) {
        TaskHandle_t t = nullptr;
        if (xTaskCreatePinnedToCoreWithCaps(play_task, "radio_play", 6144, nullptr, 5, &t, tskNO_AFFINITY,
                                            MALLOC_CAP_SPIRAM) == pdPASS) {
            s_task = t;
        }
    }
    if (!s_fft_task) {
        s_ring_w = 0;
        s_ring_r = 0;
        TaskHandle_t t = nullptr;
        if (xTaskCreatePinnedToCoreWithCaps(fft_task, "radio_fft", 6144, nullptr, 3, &t, tskNO_AFFINITY,
                                            MALLOC_CAP_SPIRAM) == pdPASS) {
            s_fft_task = t;
        }
    }
}

} // namespace

void play(const std::string &url)
{
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_url = url;
    }
    s_gen++;
    s_rate = 0;
    s_seg_format = 0;
    s_want = true;
    set_status(Status::CONNECTING);
    ensure_tasks();
    interrupt();                            /* another station while one plays */
    if (TaskHandle_t t = s_task) {
        xTaskNotifyGive(t);
    }
}

void stop()
{
    s_want = false;
    interrupt();
    if (TaskHandle_t t = s_task) {
        xTaskNotifyGive(t);
    }
}

void shutdown()
{
    s_want = false;
    s_quit = true;
    interrupt();
    if (TaskHandle_t t = s_task) {
        xTaskNotifyGive(t);
    }
    for (int i = 0; i < 150 && (s_task || s_fft_task); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task || s_fft_task) {
        ESP_LOGW(TAG, "tasks still running after 3 s");
    }
}

Status status()
{
    return static_cast<Status>(s_status.load());
}

bool busy_speaker()
{
    return s_speaker_busy;
}

void format(char *buf, size_t len)
{
    const int rate = s_rate;
    const uint32_t fmt = s_seg_format;
    if (rate <= 0) {
        buf[0] = '\0';
        return;
    }
    const char *name = fmt ? ESP_FOURCC_TO_STR(fmt) : "";
    snprintf(buf, len, "%s%s%d.%d kHz %s", name, fmt ? " · " : "", rate / 1000, rate % 1000 / 100,
             s_channels >= 2 ? "stereo" : "mono");
}

void bars(uint8_t out[BAR_COUNT])
{
    portENTER_CRITICAL(&s_bar_mux);
    memcpy(out, s_bars, BAR_COUNT);
    portEXIT_CRITICAL(&s_bar_mux);
}

} // namespace radio
