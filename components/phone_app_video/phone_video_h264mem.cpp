/*
 * The software H.264 decoder (libtinyh264, binary only) asks for its picture buffers with heap_caps_malloc_prefer /
 * heap_caps_calloc_prefer. The pictures (350-580 KB each) are in PSRAM already, but 7 work blocks ask for internal RAM
 * first: 54 KB at 640x360, 66 KB at 800x480. Linked with --wrap: while the video player task asks, blocks of 1 KB
 * or more come from PSRAM instead. Measured: decoding ~6-7% slower (640x360 22.8 -> 24.3 ms a frame), SRAM free
 * during an MP4 stays as before the video (lowest 125 KB instead of 53 KB).
 */
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" TaskHandle_t phone_video_task(void);

namespace {

constexpr size_t TO_PSRAM_FROM = 1024;
constexpr int MAX_CAPS = 4;

size_t read_caps(size_t num, va_list ap, uint32_t *caps)
{
    size_t n = 0;
    for (size_t i = 0; i < num; i++) {
        const uint32_t c = va_arg(ap, uint32_t);
        if (n < MAX_CAPS) {
            caps[n++] = c;
        }
    }
    return n;
}

bool to_psram(size_t size)
{
    return size >= TO_PSRAM_FROM && xTaskGetCurrentTaskHandle() == phone_video_task();
}

} // namespace

extern "C" void *__wrap_heap_caps_malloc_prefer(size_t size, size_t num, ...)
{
    uint32_t caps[MAX_CAPS];
    va_list ap;
    va_start(ap, num);
    const size_t n = read_caps(num, ap, caps);
    va_end(ap);
    if (to_psram(size)) {
        void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            return p;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (void *p = heap_caps_malloc(size, caps[i])) {
            return p;
        }
    }
    return nullptr;
}

extern "C" void *__wrap_heap_caps_calloc_prefer(size_t count, size_t size, size_t num, ...)
{
    uint32_t caps[MAX_CAPS];
    va_list ap;
    va_start(ap, num);
    const size_t n = read_caps(num, ap, caps);
    va_end(ap);
    if (to_psram(count * size)) {
        void *p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            return p;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (void *p = heap_caps_calloc(count, size, caps[i])) {
            return p;
        }
    }
    return nullptr;
}
