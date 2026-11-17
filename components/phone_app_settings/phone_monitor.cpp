#include "phone_monitor.h"
#include <cstdio>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "phone_fonts.h"
#include "phone_locale.h"

namespace {

const char *TAG = "MON";
constexpr uint32_t PERIOD_MS = 1000;
constexpr int LOG_EVERY = 5;            // periods

lv_obj_t *s_box;
lv_obj_t *s_text;
lv_timer_t *s_timer;
uint32_t s_frames;
int64_t s_last_us;
uint32_t s_last_idle[configNUMBER_OF_CORES];
int s_ticks;

void on_render(lv_event_t *)
{
    s_frames++;
}

/* Load of each core over the last period, from the run time of its idle task (microseconds) */
void cpu_load(int *load)
{
    const int64_t now = esp_timer_get_time();
    const uint32_t elapsed = static_cast<uint32_t>(now - s_last_us);
    s_last_us = now;
    for (int c = 0; c < configNUMBER_OF_CORES; c++) {
        const uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(c);
        const uint32_t d = idle - s_last_idle[c];
        s_last_idle[c] = idle;
        int pct = elapsed ? 100 - static_cast<int>(static_cast<uint64_t>(d) * 100 / elapsed) : 0;
        load[c] = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    }
}

void update(lv_timer_t *)
{
    int load[configNUMBER_OF_CORES] = {};
    cpu_load(load);
    const uint32_t fps = s_frames * 1000 / PERIOD_MS;
    s_frames = 0;

    constexpr uint32_t INT = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t DMA = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    const size_t sram = heap_caps_get_free_size(INT) / 1024;
    const size_t sram_min = heap_caps_get_minimum_free_size(INT) / 1024;
    const size_t sram_big = heap_caps_get_largest_free_block(INT) / 1024;
    const size_t dma = heap_caps_get_free_size(DMA) / 1024;
    const size_t dma_big = heap_caps_get_largest_free_block(DMA) / 1024;
    const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    const size_t sram_total = heap_caps_get_total_size(INT) / 1024;
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024;
    const size_t psram_big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024;

    lv_label_set_text_fmt(s_text,
                          "CPU   %d%% %d%%  FPS %u\n"
                          "SRAM  %u/%uK min %uK max %uK\n"
                          "DMA   %uK max %uK\n"
                          "PSRAM %u.%u/%u.%uM max %u.%uM",
                          load[0], configNUMBER_OF_CORES > 1 ? load[1] : 0, static_cast<unsigned>(fps),
                          static_cast<unsigned>(sram), static_cast<unsigned>(sram_total), static_cast<unsigned>(sram_min),
                          static_cast<unsigned>(sram_big),
                          static_cast<unsigned>(dma), static_cast<unsigned>(dma_big),
                          static_cast<unsigned>(psram / 1024), static_cast<unsigned>(psram % 1024 * 10 / 1024),
                          static_cast<unsigned>(psram_total / 1024), static_cast<unsigned>(psram_total % 1024 * 10 / 1024),
                          static_cast<unsigned>(psram_big / 1024), static_cast<unsigned>(psram_big % 1024 * 10 / 1024));
    lv_obj_move_foreground(s_box);

    if (++s_ticks >= LOG_EVERY) {
        s_ticks = 0;
        ESP_LOGI(TAG, "cpu %d%% %d%% fps %u | sram %uK min %uK max %uK | dma %uK max %uK | psram %uK max %uK",
                 load[0], configNUMBER_OF_CORES > 1 ? load[1] : 0, static_cast<unsigned>(fps),
                 static_cast<unsigned>(sram), static_cast<unsigned>(sram_min), static_cast<unsigned>(sram_big),
                 static_cast<unsigned>(dma), static_cast<unsigned>(dma_big),
                 static_cast<unsigned>(psram), static_cast<unsigned>(psram_big));
    }
}

void show()
{
    if (s_box) {
        return;
    }
    /* On the system layer, over apps, the lock screen and the menus; touches go through */
    s_box = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_box, lv_color_black(), 0);
    lv_obj_set_style_radius(s_box, 8, 0);
    lv_obj_set_style_pad_all(s_box, 6, 0);
    lv_obj_remove_flag(s_box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_align(s_box, LV_ALIGN_TOP_RIGHT, -6, 40);
    s_text = lv_label_create(s_box);
    lv_obj_set_style_text_font(s_text, phone_font_by_size(14), 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(0x7CFC9A), 0);
    lv_label_set_text(s_text, "...");

    s_frames = 0;
    s_ticks = 0;
    s_last_us = esp_timer_get_time();
    for (int c = 0; c < configNUMBER_OF_CORES; c++) {
        s_last_idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
    }
    lv_display_add_event_cb(lv_display_get_default(), on_render, LV_EVENT_RENDER_READY, nullptr);
    s_timer = lv_timer_create(update, PERIOD_MS, nullptr);
    ESP_LOGI(TAG, "on");
}

void hide()
{
    if (!s_box) {
        return;
    }
    lv_timer_delete(s_timer);
    s_timer = nullptr;
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), on_render, nullptr);
    lv_obj_delete(s_box);
    s_box = nullptr;
    s_text = nullptr;
    ESP_LOGI(TAG, "off");
}

} // namespace

extern "C" void phone_monitor_init(void)
{
    if (phone_pref_get("mon", 0)) {
        show();
    }
}

extern "C" void phone_monitor_set(bool on)
{
    phone_pref_set("mon", on ? 1 : 0);
    if (on) {
        show();
    } else {
        hide();
    }
}

extern "C" bool phone_monitor_is_on(void)
{
    return s_box != nullptr;
}
