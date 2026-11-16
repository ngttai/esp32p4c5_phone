#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "assistant_emote.h"
extern "C" {
#include "gfx_eaf_dec.h"
}

namespace {

const char *TAG = "assistant_emote";

#define EMOTE_ASSET(name) \
    extern const uint8_t name##_start[] asm("_binary_" #name "_eaf_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_eaf_end");

EMOTE_ASSET(pack_smile)
EMOTE_ASSET(pack_smile_static)
EMOTE_ASSET(pack_question)
EMOTE_ASSET(pack_sad)
EMOTE_ASSET(pack_cry)
EMOTE_ASSET(pack_angry)
EMOTE_ASSET(pack_shocked)
EMOTE_ASSET(pack_asleep)

struct Asset {
    const uint8_t *start;
    const uint8_t *end;
};

const Asset SMILE = {pack_smile_start, pack_smile_end};
const Asset SMILE_STATIC = {pack_smile_static_start, pack_smile_static_end};
const Asset QUESTION = {pack_question_start, pack_question_end};
const Asset SAD = {pack_sad_start, pack_sad_end};
const Asset CRY = {pack_cry_start, pack_cry_end};
const Asset ANGRY = {pack_angry_start, pack_angry_end};
const Asset SHOCKED = {pack_shocked_start, pack_shocked_end};
const Asset ASLEEP = {pack_asleep_start, pack_asleep_end};

/* Emotion names of the Xiaozhi protocol; same mapping as emote.json of esp_chatbot_v1 */
const struct {
    const char *emotion;
    const Asset *asset;
    int fps;
} MAP[] = {
    {"neutral", &SMILE_STATIC, 20}, {"idle", &SMILE, 20},
    {"happy", &SMILE, 20}, {"laughing", &SMILE, 20}, {"funny", &SMILE, 20}, {"loving", &SMILE, 20},
    {"confident", &SMILE, 20}, {"delicious", &SMILE, 20}, {"silly", &SMILE, 20}, {"winking", &SMILE, 20},
    {"relaxed", &SMILE, 20}, {"cool", &SMILE, 20}, {"kissy", &SMILE, 20},
    {"embarrassed", &QUESTION, 20}, {"thinking", &QUESTION, 20}, {"confused", &QUESTION, 20},
    {"sad", &SAD, 20}, {"crying", &CRY, 20}, {"angry", &ANGRY, 20},
    {"surprised", &SHOCKED, 20}, {"shocked", &SHOCKED, 20}, {"sleepy", &ASLEEP, 5},
};

constexpr int SIZE = ASSISTANT_EMOTE_SIZE;

lv_obj_t *s_img;
lv_timer_t *s_timer;
lv_image_dsc_t s_dsc;
uint16_t *s_frame;
uint16_t s_background;
eaf_dec_handle_t s_dec;
const Asset *s_asset;
int s_index, s_total;

uint16_t rgb565(uint32_t hex)
{
    return static_cast<uint16_t>(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F));
}

/* Decode the next valid frame; the flat color around the eyes becomes the app background */
void show_next()
{
    if (!s_dec || !s_frame || s_total <= 0) {
        return;
    }
    for (int tries = 0; tries < s_total; tries++) {
        const int index = s_index;
        s_index = (s_index + 1) % s_total;
        eaf_dec_header_t header;
        if (eaf_dec_get_frame_info(s_dec, index, &header) != EAF_DEC_TYPE_VALID) {
            continue;       /* the pack ends with a marker frame */
        }
        const bool fits = header.width == SIZE && header.height == SIZE;
        eaf_dec_free_header(&header);
        if (!fits || eaf_dec_decode_frame(s_dec, index, reinterpret_cast<uint8_t *>(s_frame),
                                          static_cast<size_t>(SIZE) * SIZE * 2, false) != ESP_OK) {
            continue;
        }
        const uint16_t key = s_frame[0];
        if (key != s_background) {
            for (int i = 0; i < SIZE * SIZE; i++) {
                if (s_frame[i] == key) {
                    s_frame[i] = s_background;
                }
            }
        }
        lv_obj_invalidate(s_img);
        return;
    }
}

void on_timer(lv_timer_t *)
{
    show_next();
}

} // namespace

extern "C" lv_obj_t *assistant_emote_create(lv_obj_t *parent, uint32_t background)
{
    s_background = rgb565(background);
    if (!s_frame) {
        s_frame = static_cast<uint16_t *>(heap_caps_malloc(static_cast<size_t>(SIZE) * SIZE * 2, MALLOC_CAP_SPIRAM));
    }
    if (s_frame) {
        for (int i = 0; i < SIZE * SIZE; i++) {
            s_frame[i] = s_background;
        }
    } else {
        ESP_LOGE(TAG, "no memory for the frame");
    }
    s_dsc = {};
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = SIZE;
    s_dsc.header.h = SIZE;
    s_dsc.header.stride = SIZE * 2;
    s_dsc.data_size = SIZE * SIZE * 2;
    s_dsc.data = reinterpret_cast<const uint8_t *>(s_frame);

    s_img = lv_image_create(parent);
    lv_obj_set_size(s_img, SIZE, SIZE);
    if (s_frame) {
        lv_image_set_src(s_img, &s_dsc);
    }
    s_asset = nullptr;
    s_timer = lv_timer_create(on_timer, 50, nullptr);
    assistant_emote_set("neutral");
    return s_img;
}

extern "C" void assistant_emote_set(const char *emotion)
{
    if (!s_img) {
        return;
    }
    const Asset *asset = &SMILE_STATIC;
    int fps = 20;
    for (const auto &m : MAP) {
        if (emotion && strcmp(emotion, m.emotion) == 0) {
            asset = m.asset;
            fps = m.fps;
            break;
        }
    }
    if (asset == s_asset) {
        return;
    }
    if (s_dec) {
        eaf_dec_deinit(s_dec);
        s_dec = nullptr;
    }
    s_asset = asset;
    s_index = 0;
    s_total = 0;
    if (eaf_dec_init(asset->start, asset->end - asset->start, &s_dec) == ESP_OK) {
        s_total = eaf_dec_get_total_frames(s_dec);
    } else {
        ESP_LOGE(TAG, "cannot open the animation of \"%s\"", emotion ? emotion : "");
        s_dec = nullptr;
    }
    lv_timer_set_period(s_timer, 1000 / fps);
    show_next();
}

extern "C" void assistant_emote_destroy(void)
{
    /* the timer was created while the app was starting: the Brookesia core deletes it with the app */
    s_timer = nullptr;
    if (s_dec) {
        eaf_dec_deinit(s_dec);
        s_dec = nullptr;
    }
    s_img = nullptr;
    s_asset = nullptr;
    heap_caps_free(s_frame);
    s_frame = nullptr;
}
