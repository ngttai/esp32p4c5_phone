#include "phone_storage.h"
#include <atomic>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"

#define TAG "SD"

namespace {

std::atomic<int> s_state{PHONE_STORAGE_DISABLED};

void mount_task(void *)
{
    /* The BSP never formats the card when mounting fails */
    esp_err_t err = bsp_sdcard_mount();
    s_state = err == ESP_OK ? PHONE_STORAGE_MOUNTED : PHONE_STORAGE_NO_CARD;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No microSD card mounted: %s", esp_err_to_name(err));
    }
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void phone_storage_start(void)
{
#if CONFIG_PHONE_SD_CARD_MOUNT
    if (s_state != PHONE_STORAGE_DISABLED) {
        return;
    }
    s_state = PHONE_STORAGE_CHECKING;
    if (xTaskCreate(mount_task, "sd_mount", 4096, nullptr, 3, nullptr) != pdPASS) {
        s_state = PHONE_STORAGE_NO_CARD;
    }
#endif
}

extern "C" phone_storage_state_t phone_storage_get(uint64_t *total, uint64_t *free)
{
    auto state = static_cast<phone_storage_state_t>(s_state.load());
    if (state == PHONE_STORAGE_MOUNTED) {
        uint64_t t = 0, f = 0;
        if (esp_vfs_fat_info(CONFIG_BSP_SD_MOUNT_POINT, &t, &f) != ESP_OK) {
            t = f = 0;
        }
        if (total) {
            *total = t;
        }
        if (free) {
            *free = f;
        }
    }
    return state;
}
