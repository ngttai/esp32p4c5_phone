#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "phone_storage.h"
#include "assistant_emote.h"
extern "C" {
#include "gfx_eaf_dec.h"
}

namespace {

const char *TAG = "assistant_emote";

#define EMOTE_ASSET(name) \
    extern const uint8_t name##_start[] asm("_binary_pack_" #name "_eaf_start"); \
    extern const uint8_t name##_end[] asm("_binary_pack_" #name "_eaf_end");

EMOTE_ASSET(smile_static)
EMOTE_ASSET(idle)
EMOTE_ASSET(happy)
EMOTE_ASSET(laugh)
EMOTE_ASSET(snigger)
EMOTE_ASSET(loving)
EMOTE_ASSET(confident)
EMOTE_ASSET(yummy)
EMOTE_ASSET(silly)
EMOTE_ASSET(winking)
EMOTE_ASSET(relaxed)
EMOTE_ASSET(leisure)
EMOTE_ASSET(shy)
EMOTE_ASSET(ponder)
EMOTE_ASSET(confused)
EMOTE_ASSET(sad)
EMOTE_ASSET(cry)
EMOTE_ASSET(angry)
EMOTE_ASSET(surprised)
EMOTE_ASSET(shocked)
EMOTE_ASSET(asleep)

#define PACK(name) #name, name##_start, name##_end

/* Emotion names of the Xiaozhi protocol -> animation of Espressif's Emote Pack (eaf_360x360), embedded */
const struct {
    const char *emotion;
    const char *pack;           // the name in the pack: pack_<name>.eaf
    const uint8_t *start;
    const uint8_t *end;
    int fps;
} MAP[] = {
    {"neutral", PACK(smile_static), 20}, {"idle", PACK(idle), 20},
    {"happy", PACK(happy), 20}, {"laughing", PACK(laugh), 20}, {"funny", PACK(snigger), 20},
    {"loving", PACK(loving), 20}, {"kissy", PACK(loving), 20}, {"confident", PACK(confident), 20},
    {"delicious", PACK(yummy), 20}, {"silly", PACK(silly), 20}, {"winking", PACK(winking), 20},
    {"relaxed", PACK(relaxed), 20}, {"cool", PACK(leisure), 20}, {"embarrassed", PACK(shy), 20},
    {"thinking", PACK(ponder), 20}, {"confused", PACK(confused), 20},
    {"sad", PACK(sad), 20}, {"crying", PACK(cry), 20}, {"angry", PACK(angry), 20},
    {"surprised", PACK(surprised), 20}, {"shocked", PACK(shocked), 20}, {"sleepy", PACK(asleep), 5},
};

/* A card can bring its own faces: /sdcard/assistant/emote/<emotion>.eaf, else pack_<name>.eaf (a folder of the
 * Emote Pack copied as it is); any size up to MAX_SIDE, shown scaled into the SIZE square */
constexpr const char *SD_DIR = "/sdcard/assistant/emote";
constexpr int MAX_SIDE = 480;
constexpr size_t MAX_FILE = 4 * 1024 * 1024;

constexpr int SIZE = ASSISTANT_EMOTE_SIZE;

lv_obj_t *s_img;
lv_timer_t *s_timer;
lv_image_dsc_t s_dsc;
uint16_t *s_frame;
size_t s_frame_px;              // pixels s_frame holds
int s_w, s_h;                   // size of the animation playing
uint16_t s_background;
eaf_dec_handle_t s_dec;
uint8_t *s_file;                // the animation read from the card (PSRAM), or nullptr when embedded
char s_emotion[32];
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
        const bool fits = header.width == s_w && header.height == s_h;
        eaf_dec_free_header(&header);
        if (!fits || eaf_dec_decode_frame(s_dec, index, reinterpret_cast<uint8_t *>(s_frame),
                                          static_cast<size_t>(s_w) * s_h * 2, false) != ESP_OK) {
            continue;
        }
        const uint16_t key = s_frame[0];
        if (key != s_background) {
            for (int i = 0; i < s_w * s_h; i++) {
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

/* The whole file in PSRAM, or nullptr when it is not there */
uint8_t *read_card(const char *name, size_t *size)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.eaf", SD_DIR, name);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) > MAX_FILE) {
        return nullptr;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return nullptr;
    }
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM));
    const bool ok = buf && fread(buf, 1, st.st_size, f) == static_cast<size_t>(st.st_size);
    fclose(f);
    if (!ok) {
        heap_caps_free(buf);
        return nullptr;
    }
    *size = st.st_size;
    ESP_LOGI(TAG, "%s from the card (%u bytes)", path, static_cast<unsigned>(*size));
    return buf;
}

bool card_mounted()
{
    uint64_t total = 0, free_bytes = 0;
    return phone_storage_get(&total, &free_bytes) == PHONE_STORAGE_MOUNTED;
}

void close_animation()
{
    if (s_dec) {
        eaf_dec_deinit(s_dec);
        s_dec = nullptr;
    }
    heap_caps_free(s_file);
    s_file = nullptr;
    s_total = 0;
}

/* Open data as the animation playing; false when it cannot be decoded or is too big */
bool open_animation(const uint8_t *data, size_t size)
{
    if (eaf_dec_init(data, size, &s_dec) != ESP_OK) {
        s_dec = nullptr;
        return false;
    }
    s_total = eaf_dec_get_total_frames(s_dec);
    int w = 0, h = 0;
    for (int i = 0; i < s_total && !w; i++) {
        eaf_dec_header_t header;
        if (eaf_dec_get_frame_info(s_dec, i, &header) == EAF_DEC_TYPE_VALID) {
            w = header.width;
            h = header.height;
            eaf_dec_free_header(&header);
        }
    }
    if (w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE) {
        ESP_LOGW(TAG, "animation of %dx%d not shown", w, h);
        eaf_dec_deinit(s_dec);
        s_dec = nullptr;
        return false;
    }
    const size_t px = static_cast<size_t>(w) * h;
    if (px > s_frame_px) {
        auto *frame = static_cast<uint16_t *>(heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM));
        if (!frame) {
            eaf_dec_deinit(s_dec);
            s_dec = nullptr;
            return false;
        }
        heap_caps_free(s_frame);
        s_frame = frame;
        s_frame_px = px;
    }
    if (w != s_w || h != s_h || s_dsc.data != reinterpret_cast<const uint8_t *>(s_frame)) {
        s_w = w;
        s_h = h;
        s_dsc.header.w = w;
        s_dsc.header.h = h;
        s_dsc.header.stride = w * 2;
        s_dsc.data_size = px * 2;
        s_dsc.data = reinterpret_cast<const uint8_t *>(s_frame);
        for (size_t i = 0; i < px; i++) {
            s_frame[i] = s_background;
        }
        lv_image_set_src(s_img, &s_dsc);
        lv_image_set_scale(s_img, LV_SCALE_NONE * SIZE / (w > h ? w : h));
    }
    return true;
}

} // namespace

extern "C" lv_obj_t *assistant_emote_create(lv_obj_t *parent, uint32_t background)
{
    s_background = rgb565(background);
    eaf_dec_set_transparent_color(s_background);     // the pack draws on a cleared background
    s_dsc = {};
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_w = s_h = 0;
    s_img = lv_image_create(parent);
    lv_obj_set_size(s_img, SIZE, SIZE);
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CENTER);
    s_emotion[0] = '\0';
    s_timer = lv_timer_create(on_timer, 50, nullptr);
    assistant_emote_set("neutral");
    return s_img;
}

extern "C" void assistant_emote_set(const char *emotion)
{
    if (!s_img) {
        return;
    }
    int m = 0;      /* unknown names play "neutral", the first entry */
    for (int i = 0; i < static_cast<int>(sizeof(MAP) / sizeof(MAP[0])); i++) {
        if (emotion && strcmp(emotion, MAP[i].emotion) == 0) {
            m = i;
            break;
        }
    }
    if (strcmp(s_emotion, MAP[m].emotion) == 0 && s_dec) {
        return;
    }
    strlcpy(s_emotion, MAP[m].emotion, sizeof(s_emotion));
    close_animation();
    s_index = 0;

    bool ok = false;
    if (card_mounted()) {
        char pack[40];
        snprintf(pack, sizeof(pack), "pack_%s", MAP[m].pack);
        size_t size = 0;
        s_file = read_card(MAP[m].emotion, &size);
        if (!s_file) {
            s_file = read_card(pack, &size);
        }
        ok = s_file && open_animation(s_file, size);
        if (s_file && !ok) {
            ESP_LOGW(TAG, "the card's animation of \"%s\" cannot be used: the built-in one plays", MAP[m].emotion);
            close_animation();
        }
    }
    if (!ok && !open_animation(MAP[m].start, MAP[m].end - MAP[m].start)) {
        ESP_LOGE(TAG, "cannot open the animation of \"%s\"", MAP[m].emotion);
    }
    lv_timer_set_period(s_timer, 1000 / MAP[m].fps);
    show_next();
}

extern "C" void assistant_emote_destroy(void)
{
    /* the timer was created while the app was starting: the Brookesia core deletes it with the app */
    s_timer = nullptr;
    close_animation();
    s_img = nullptr;
    s_emotion[0] = '\0';
    heap_caps_free(s_frame);
    s_frame = nullptr;
    s_frame_px = 0;
    s_w = s_h = 0;
}
