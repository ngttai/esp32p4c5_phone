#include "phone_audio.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"

#define TAG "AUD"

namespace {

constexpr int RATE = 16000;
constexpr int TONE_HZ = 1000;
constexpr int TONE_MS = 150;
constexpr int SAMPLES = RATE * TONE_MS / 1000;

std::atomic<int> s_volume{60};
std::atomic<bool> s_busy{false};
esp_codec_dev_handle_t s_spk;   // only touched by the beep task, one at a time (s_busy)

void beep_task(void *)
{
    static int16_t tone[SAMPLES];
    if (!s_spk) {
        s_spk = bsp_audio_codec_speaker_init();
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = RATE;
    fs.channel = 1;
    fs.bits_per_sample = 16;
    /* The codec is opened only for the tone, so the speaker amplifier stays off otherwise */
    if (s_spk && esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_out_vol(s_spk, s_volume.load());
        for (int i = 0; i < SAMPLES; i++) {
            /* 10 ms fade in/out avoids clicks */
            int edge = RATE / 100;
            float gain = i < edge ? (float)i / edge : (i > SAMPLES - edge ? (float)(SAMPLES - i) / edge : 1.0f);
            tone[i] = (int16_t)(12000.0f * gain * sinf(2.0f * (float)M_PI * TONE_HZ * i / RATE));
        }
        esp_codec_dev_write(s_spk, tone, sizeof(tone));
        esp_codec_dev_close(s_spk);
    } else {
        ESP_LOGW(TAG, "Speaker not available");
    }
    s_busy = false;
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void phone_audio_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

extern "C" int phone_audio_get_volume(void)
{
    return s_volume;
}

extern "C" void phone_audio_beep(void)
{
    if (s_volume.load() == 0 || s_busy.exchange(true)) {
        return;
    }
    if (xTaskCreate(beep_task, "beep", 4096, nullptr, 4, nullptr) != pdPASS) {
        s_busy = false;
    }
}
