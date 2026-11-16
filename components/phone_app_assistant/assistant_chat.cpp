/*
 * Voice session of the assistant.
 *
 * Two tasks:
 *   - the session task owns the esp_xiaozhi chat handle: it runs the commands (button, server
 *     events) and, while a conversation is open, reads the microphone in 60 ms frames, encodes
 *     them with Opus and sends them while the assistant listens;
 *   - the play task decodes the Opus packets of the answer and writes them to the speaker.
 *
 * The conversation is half duplex: nothing is sent while the answer plays, so no echo
 * cancellation is needed. After the answer, the assistant listens again by itself ("auto" mode of
 * the protocol) until the button ends the conversation or the server says goodbye.
 *
 * The microphone and the speaker share the I2S clock, so both run at 16 kHz; the Opus decoder
 * resamples the answer, whatever rate the server encodes it at.
 */
#include <atomic>
#include <cstdlib>
#include <cstring>
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_xiaozhi_chat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "phone_audio.h"
#include "phone_locale.h"
#include "assistant_chat.h"

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "assistant_chat";

constexpr int RATE = 16000;
constexpr int FRAME_MS = 60;
constexpr int FRAME_SAMPLES = RATE * FRAME_MS / 1000;
constexpr int OPUS_BITRATE = 24000;
constexpr float MIC_GAIN_DB = 30.0f;
constexpr int AUDIO_QUEUE_LEN = 80;         // about 5 s of answer waiting to be played
constexpr size_t PCM_BYTES = 8192;          // one decoded packet (up to 120 ms at 16 kHz, with room)

enum Cmd {
    CMD_BUTTON, CMD_CONNECTED, CMD_DISCONNECTED, CMD_CHANNEL_OPENED, CMD_CHANNEL_CLOSED, CMD_GOODBYE,
    CMD_TTS_START, CMD_TTS_STOP, CMD_STOP,
};

struct Packet {
    uint8_t *data;
    int len;
};

std::atomic<int> s_state{ASSISTANT_CHAT_OFF};
std::atomic<int> s_mic_level;
std::atomic<int> s_queued;                  // packets waiting in s_audio or being played
QueueHandle_t s_cmds;
QueueHandle_t s_msgs;
QueueHandle_t s_audio;
SemaphoreHandle_t s_done;
SemaphoreHandle_t s_spk_lock;
TaskHandle_t s_session;
TaskHandle_t s_player;
std::atomic<bool> s_player_run;
std::atomic<bool> s_player_done;
bool s_has_mqtt, s_has_websocket;

/* session task only */
esp_xiaozhi_chat_handle_t s_chat;
esp_codec_dev_handle_t s_spk;               // also read by the play task, under s_spk_lock
esp_codec_dev_handle_t s_mic;
bool s_spk_open, s_mic_open;
bool s_talking;                             // a conversation is open: codecs are open, the microphone is read
bool s_answer_done;                         // the server finished the answer; listen again once it has been played
bool s_goodbye;                             // the server ended the conversation; stop once the answer has been played
bool s_connected;                           // link to the server, as last reported
void *s_enc;
int s_enc_in, s_enc_out;
uint8_t *s_enc_buf;
int16_t *s_mic_buf;

void post(Cmd cmd)
{
    if (s_cmds) {
        const int c = cmd;
        xQueueSend(s_cmds, &c, 0);
    }
}

void push_msg(assistant_chat_msg_type_t type, const char *text)
{
    if (!s_msgs || !text) {
        return;
    }
    auto *msg = static_cast<assistant_chat_msg_t *>(malloc(sizeof(assistant_chat_msg_t)));
    if (!msg) {
        return;
    }
    msg->type = type;
    strlcpy(msg->text, text, sizeof(msg->text));
    if (xQueueSend(s_msgs, &msg, 0) != pdPASS) {
        free(msg);
    }
}

void flush_audio()
{
    Packet p;
    while (s_audio && xQueueReceive(s_audio, &p, 0) == pdPASS) {
        free(p.data);
        s_queued--;
    }
}

/* ---- callbacks of esp_xiaozhi (its own tasks) ---- */

void on_audio(const uint8_t *data, int len, void *)
{
    if (len <= 0 || s_state != ASSISTANT_CHAT_SPEAKING) {
        return;
    }
    Packet p = {static_cast<uint8_t *>(malloc(len)), len};
    if (!p.data) {
        return;
    }
    memcpy(p.data, data, len);
    s_queued++;
    if (xQueueSend(s_audio, &p, 0) != pdPASS) {
        free(p.data);
        s_queued--;
    }
}

void on_chat_event(esp_xiaozhi_chat_event_t event, void *event_data, void *)
{
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        auto *text = static_cast<esp_xiaozhi_chat_text_data_t *>(event_data);
        if (text && text->text) {
            const bool user = text->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER;
            push_msg(user ? ASSISTANT_CHAT_MSG_USER : ASSISTANT_CHAT_MSG_ASSISTANT, text->text);
            int listening = ASSISTANT_CHAT_LISTENING;
            if (user) {
                s_state.compare_exchange_strong(listening, ASSISTANT_CHAT_THINKING);
            }
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
        push_msg(ASSISTANT_CHAT_MSG_EMOTION, static_cast<const char *>(event_data));
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        auto *tts = static_cast<esp_xiaozhi_chat_tts_state_t *>(event_data);
        if (tts && tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            post(CMD_TTS_START);
        } else if (tts && tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            post(CMD_TTS_STOP);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR:
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Máy chủ báo lỗi", "The server reported an error"));
        break;
    default:
        break;
    }
}

void on_link_event(void *, esp_event_base_t, int32_t id, void *)
{
    switch (id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        post(CMD_CONNECTED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        post(CMD_DISCONNECTED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        post(CMD_CHANNEL_OPENED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        post(CMD_CHANNEL_CLOSED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        post(CMD_GOODBYE);
        break;
    default:
        break;
    }
}

/* ---- play task ---- */

void play_task(void *)
{
    esp_opus_dec_cfg_t cfg = {};
    cfg.sample_rate = RATE;
    cfg.channel = 1;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    cfg.self_delimited = false;
    void *dec = nullptr;
    auto *pcm = static_cast<uint8_t *>(malloc(PCM_BYTES));
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &dec) != ESP_AUDIO_ERR_OK || !pcm) {
        ESP_LOGE(TAG, "no Opus decoder");
        dec = nullptr;
    }
    int volume = -1;
    while (s_player_run) {
        Packet p;
        if (xQueueReceive(s_audio, &p, pdMS_TO_TICKS(100)) != pdPASS) {
            continue;
        }
        if (dec) {
            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = p.data;
            raw.len = p.len;
            esp_audio_dec_out_frame_t out = {};
            out.buffer = pcm;
            out.len = PCM_BYTES;
            esp_audio_dec_info_t info = {};
            if (esp_opus_dec_decode(dec, &raw, &out, &info) == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
                xSemaphoreTake(s_spk_lock, portMAX_DELAY);
                if (s_spk_open) {
                    const int v = phone_audio_get_volume();
                    if (v != volume) {
                        volume = v;
                        esp_codec_dev_set_out_vol(s_spk, v);
                    }
                    esp_codec_dev_write(s_spk, pcm, out.decoded_size);
                } else {
                    volume = -1;
                }
                xSemaphoreGive(s_spk_lock);
            }
        }
        free(p.data);
        s_queued--;
    }
    if (dec) {
        esp_opus_dec_close(dec);
    }
    free(pcm);
    s_player_done = true;
    vTaskDelete(nullptr);
}

/* ---- session task ---- */

void close_codecs()
{
    if (s_mic_open) {
        esp_codec_dev_close(s_mic);
        s_mic_open = false;
    }
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    if (s_spk_open) {
        esp_codec_dev_close(s_spk);
        s_spk_open = false;
    }
    xSemaphoreGive(s_spk_lock);
    if (s_spk) {
        phone_audio_speaker_give();
        s_spk = nullptr;
    }
}

/* Speaker and microphone at 16 kHz mono; false when the speaker is busy (music) or a codec fails */
bool open_codecs()
{
    s_spk = static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take());
    if (!s_spk) {
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Loa đang được dùng (nhạc?). Hãy dừng rồi thử lại.",
                                              "The speaker is in use (music?). Stop it and try again."));
        return false;
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = RATE;
    fs.channel = 1;
    fs.bits_per_sample = 16;
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    s_spk_open = esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK;
    if (s_spk_open) {
        esp_codec_dev_set_out_vol(s_spk, phone_audio_get_volume());
    }
    xSemaphoreGive(s_spk_lock);
    s_mic = bsp_audio_codec_microphone_init();
    s_mic_open = s_mic && esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK;
    if (s_mic_open) {
        esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    }
    if (!s_spk_open || !s_mic_open) {
        ESP_LOGE(TAG, "codec open failed: speaker %d, microphone %d", s_spk_open, s_mic_open);
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Không mở được micro hoặc loa", "Could not open the microphone or the speaker"));
        close_codecs();
        return false;
    }
    return true;
}

/* Leave the conversation: back to idle, or to `state` */
void end_talk(int state)
{
    if (s_talking) {
        s_talking = false;
        flush_audio();
        close_codecs();
    }
    s_answer_done = false;
    s_goodbye = false;
    s_mic_level = 0;
    s_state = state;
}

void listen()
{
    s_answer_done = false;
    if (esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO) == ESP_OK) {
        s_state = ASSISTANT_CHAT_LISTENING;
    } else {
        /* the server closed the conversation on its side (after a goodbye, or a long silence): a normal end */
        ESP_LOGI(TAG, "the server ended the conversation");
        esp_xiaozhi_chat_close_audio_channel(s_chat);
        end_talk(ASSISTANT_CHAT_IDLE);
    }
}

void on_button()
{
    switch (s_state.load()) {
    case ASSISTANT_CHAT_IDLE: {
        if (!open_codecs()) {
            return;
        }
        s_talking = true;
        s_state = ASSISTANT_CHAT_OPENING;
        esp_xiaozhi_chat_audio_t audio = {};
        audio.format = "opus";
        audio.sample_rate = RATE;
        audio.channels = 1;
        audio.frame_duration = FRAME_MS;
        const esp_err_t err = esp_xiaozhi_chat_open_audio_channel(s_chat, &audio, nullptr, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "open_audio_channel: %s", esp_err_to_name(err));
            push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Không mở được kênh thoại", "Could not open the voice channel"));
            end_talk(ASSISTANT_CHAT_IDLE);
        }
        break;
    }
    case ASSISTANT_CHAT_OPENING:
    case ASSISTANT_CHAT_LISTENING:
    case ASSISTANT_CHAT_THINKING:
        esp_xiaozhi_chat_close_audio_channel(s_chat);
        end_talk(ASSISTANT_CHAT_IDLE);
        break;
    case ASSISTANT_CHAT_SPEAKING:
        /* cut the answer and listen again */
        esp_xiaozhi_chat_send_abort_speaking(s_chat, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
        flush_audio();
        listen();
        break;
    default:
        break;
    }
}

/* One microphone frame: sent while listening, dropped otherwise (the clock keeps running) */
void pump_microphone()
{
    if (esp_codec_dev_read(s_mic, s_mic_buf, FRAME_SAMPLES * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    if (s_state != ASSISTANT_CHAT_LISTENING || !s_enc) {
        s_mic_level = 0;
        return;
    }
    int peak = 0;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        const int v = s_mic_buf[i] < 0 ? -s_mic_buf[i] : s_mic_buf[i];
        peak = v > peak ? v : peak;
    }
    s_mic_level = peak * 100 / 32768;
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t *>(s_mic_buf);
    in.len = s_enc_in;
    esp_audio_enc_out_frame_t out = {};
    out.buffer = s_enc_buf;
    out.len = s_enc_out;
    if (esp_opus_enc_process(s_enc, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
        esp_xiaozhi_chat_send_audio_data(s_chat, reinterpret_cast<const char *>(s_enc_buf), out.encoded_bytes);
    }
}

bool open_encoder()
{
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = RATE;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = OPUS_BITRATE;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = 3;
    cfg.enable_fec = false;
    cfg.enable_dtx = true;
    cfg.enable_vbr = true;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_enc) != ESP_AUDIO_ERR_OK ||
            esp_opus_enc_get_frame_size(s_enc, &s_enc_in, &s_enc_out) != ESP_AUDIO_ERR_OK) {
        s_enc = nullptr;
        return false;
    }
    s_enc_buf = static_cast<uint8_t *>(malloc(s_enc_out));
    s_mic_buf = static_cast<int16_t *>(malloc(s_enc_in > static_cast<int>(FRAME_SAMPLES * sizeof(int16_t))
                                              ? s_enc_in : FRAME_SAMPLES * sizeof(int16_t)));
    ESP_LOGI(TAG, "Opus encoder: %d bytes in, up to %d bytes out per frame", s_enc_in, s_enc_out);
    return s_enc_buf && s_mic_buf && s_enc_in == static_cast<int>(FRAME_SAMPLES * sizeof(int16_t));
}

void session_task(void *)
{
    bool ok = open_encoder();
    if (!ok) {
        ESP_LOGE(TAG, "no Opus encoder");
    }
    esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, on_link_event, nullptr);
    if (ok) {
        esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
        config.audio_callback = on_audio;
        config.event_callback = on_chat_event;
        /* esp_xiaozhi prefers MQTT + UDP when it is offered both. Here the MQTT broker closes the
         * connection right after the TLS handshake, so WebSocket is used whenever the server offers it. */
        config.has_websocket_config = s_has_websocket;
        config.has_mqtt_config = s_has_mqtt && !s_has_websocket;
        ESP_LOGI(TAG, "transport: %s", config.has_mqtt_config ? "MQTT + UDP" : "WebSocket");
        /* esp_xiaozhi needs an MCP engine to start, even with no device tool registered yet */
        esp_mcp_t *mcp = nullptr;
        ok = esp_mcp_create(&mcp) == ESP_OK;
        config.mcp_engine = mcp;
        config.owns_mcp_engine = true;
        if (ok && esp_xiaozhi_chat_init(&config, &s_chat) != ESP_OK) {
            esp_mcp_destroy(mcp);
            s_chat = 0;
            ok = false;
        }
        if (ok && esp_xiaozhi_chat_start(s_chat) != ESP_OK) {
            ESP_LOGE(TAG, "chat start failed");
            ok = false;
        }
    }
    s_connected = false;
    s_state = ok ? ASSISTANT_CHAT_CONNECTING : ASSISTANT_CHAT_ERROR;

    bool run = true;
    while (run) {
        int cmd;
        /* while talking the microphone read paces the loop */
        while (xQueueReceive(s_cmds, &cmd, s_talking ? 0 : pdMS_TO_TICKS(100)) == pdPASS) {
            switch (cmd) {
            case CMD_STOP:
                run = false;
                break;
            case CMD_CONNECTED:
                s_connected = true;
                if (s_state == ASSISTANT_CHAT_CONNECTING || s_state == ASSISTANT_CHAT_ERROR) {
                    s_state = ASSISTANT_CHAT_IDLE;
                }
                break;
            case CMD_DISCONNECTED:
                s_connected = false;
                if (s_state != ASSISTANT_CHAT_OFF && s_chat) {
                    /* a link lost in the middle of a sentence is worth telling; the server hanging up
                     * after its last answer is not */
                    const int st = s_state;
                    if (s_talking && st == ASSISTANT_CHAT_SPEAKING) {
                        /* the server hung up while its last sentence is still playing: let it finish */
                        s_goodbye = true;
                        s_answer_done = true;
                        break;
                    }
                    if (s_talking && !s_goodbye && (st == ASSISTANT_CHAT_OPENING || st == ASSISTANT_CHAT_THINKING)) {
                        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Mất kết nối với máy chủ", "Lost the connection to the server"));
                    }
                    end_talk(ASSISTANT_CHAT_CONNECTING);     /* esp_xiaozhi connects again by itself */
                }
                break;
            case CMD_BUTTON:
                if (s_chat) {
                    on_button();
                }
                break;
            case CMD_CHANNEL_OPENED:
                if (s_state == ASSISTANT_CHAT_OPENING) {
                    listen();
                }
                break;
            case CMD_GOODBYE:
                /* let the goodbye sentence play to its end */
                if (s_talking && s_state == ASSISTANT_CHAT_SPEAKING) {
                    s_goodbye = true;
                    break;
                }
                [[fallthrough]];
            case CMD_CHANNEL_CLOSED:
                if (s_talking && !(s_goodbye && s_state == ASSISTANT_CHAT_SPEAKING)) {
                    end_talk(ASSISTANT_CHAT_IDLE);
                }
                break;
            case CMD_TTS_START:
                if (s_talking) {
                    s_answer_done = false;
                    s_state = ASSISTANT_CHAT_SPEAKING;
                }
                break;
            case CMD_TTS_STOP:
                if (s_state == ASSISTANT_CHAT_SPEAKING) {
                    s_answer_done = true;
                }
                break;
            default:
                break;
            }
            if (!run) {
                break;
            }
        }
        if (!run) {
            break;
        }
        if (s_talking && s_mic_open) {
            pump_microphone();
            /* the whole answer has been played: listen again */
            if (s_answer_done && s_queued <= 0 && s_state == ASSISTANT_CHAT_SPEAKING) {
                if (s_goodbye) {
                    if (s_connected) {
                        esp_xiaozhi_chat_close_audio_channel(s_chat);
                    }
                    end_talk(s_connected ? ASSISTANT_CHAT_IDLE : ASSISTANT_CHAT_CONNECTING);
                } else {
                    listen();
                }
            }
        }
    }

    if (s_chat) {
        if (s_talking) {
            esp_xiaozhi_chat_close_audio_channel(s_chat);
        }
        end_talk(ASSISTANT_CHAT_OFF);
        esp_xiaozhi_chat_stop(s_chat);
        esp_xiaozhi_chat_deinit(s_chat);
        s_chat = 0;
    }
    esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, on_link_event);
    if (s_enc) {
        esp_opus_enc_close(s_enc);
        s_enc = nullptr;
    }
    free(s_enc_buf);
    free(s_mic_buf);
    s_enc_buf = nullptr;
    s_mic_buf = nullptr;
    s_state = ASSISTANT_CHAT_OFF;
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void assistant_chat_start(bool has_mqtt, bool has_websocket)
{
    if (s_session) {
        return;
    }
    if (!s_cmds) {
        s_cmds = xQueueCreate(16, sizeof(int));
        s_msgs = xQueueCreate(16, sizeof(assistant_chat_msg_t *));
        s_audio = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(Packet));
        s_done = xSemaphoreCreateBinary();
        s_spk_lock = xSemaphoreCreateMutex();
    }
    xQueueReset(s_cmds);
    s_has_mqtt = has_mqtt;
    s_has_websocket = has_websocket;
    s_queued = 0;
    s_state = ASSISTANT_CHAT_CONNECTING;
    s_player_run = true;
    s_player_done = false;
    if (xTaskCreate(play_task, "assistant_play", 16384, nullptr, 6, &s_player) != pdPASS) {
        s_player = nullptr;
        s_player_done = true;
    }
    /* Opus encoding needs a deep stack */
    if (xTaskCreate(session_task, "assistant_chat", 32768, nullptr, 5, &s_session) != pdPASS) {
        s_session = nullptr;
        s_state = ASSISTANT_CHAT_ERROR;
    }
}

extern "C" void assistant_chat_stop(void)
{
    if (s_session) {
        post(CMD_STOP);
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
            ESP_LOGE(TAG, "session task did not stop");
        }
        s_session = nullptr;
    }
    if (s_player) {
        s_player_run = false;
        for (int i = 0; i < 100 && !s_player_done; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_player = nullptr;
    }
    flush_audio();
    assistant_chat_msg_t *msg;
    while (s_msgs && xQueueReceive(s_msgs, &msg, 0) == pdPASS) {
        free(msg);
    }
    s_state = ASSISTANT_CHAT_OFF;
}

extern "C" void assistant_chat_button(void)
{
    post(CMD_BUTTON);
}

extern "C" assistant_chat_state_t assistant_chat_state(void)
{
    return static_cast<assistant_chat_state_t>(s_state.load());
}

extern "C" bool assistant_chat_poll(assistant_chat_msg_t *out)
{
    assistant_chat_msg_t *msg;
    if (!s_msgs || !out || xQueueReceive(s_msgs, &msg, 0) != pdPASS) {
        return false;
    }
    *out = *msg;
    free(msg);
    return true;
}

extern "C" int assistant_chat_mic_level(void)
{
    return s_mic_level;
}
