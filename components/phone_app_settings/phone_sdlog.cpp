#include "phone_sdlog.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "phone_locale.h"
#include "phone_paths.h"
#include "phone_storage.h"

#define TAG "SDLOG"

namespace {

constexpr size_t RING_SIZE = 128 * 1024;        // PSRAM: the boot until the card is mounted
constexpr size_t LOG_LINE_MAX = 384;                // longer lines: whole on the serial port, cut in the file
constexpr size_t CHUNK_MAX = 4096;
constexpr long FILE_MAX = 2 * 1024 * 1024;
constexpr int KEEP_FILES = 10;
constexpr int SYNC_MS = 5000;                   // fsync: what a crash or a pulled battery loses
constexpr int RETRY_MS = 5000;                  // after a failed open or write
constexpr int CLOCK_EVERY_S = 3600;             // wall clock note in the file
constexpr const char *LOG_DIR = PHONE_SD_SYSTEM "/logs";
constexpr const char *PREF_KEY = "sdlog";

vprintf_like_t s_orig;
bool s_hooked;
RingbufHandle_t s_ring;
SemaphoreHandle_t s_fmt_mtx;            // guards s_line
char s_line[LOG_LINE_MAX];
std::atomic<bool> s_on{false};
std::atomic<bool> s_paused{false};
std::atomic<uint32_t> s_dropped{0};
TaskHandle_t s_task;

/* writer task, and phone_sdlog_storage_pause under s_file_mtx */
SemaphoreHandle_t s_file_mtx;
FILE *s_file;
int s_file_no;              // number of the file of this boot, 0 before the first one
long s_file_size;
int64_t s_last_sync_us;
int64_t s_retry_at_us;
time_t s_last_clock;

/* ---------------- log hook (any task) ---------------- */

int log_hook(const char *fmt, va_list ap)
{
    if (!s_on || xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
            xSemaphoreTake(s_fmt_mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        if (s_on) {
            s_dropped++;
        }
        return s_orig(fmt, ap);
    }
    va_list ap2;
    va_copy(ap2, ap);
    const int n = vsnprintf(s_line, sizeof(s_line), fmt, ap2);
    va_end(ap2);
    int ret;
    if (n >= 0 && n < static_cast<int>(sizeof(s_line))) {
        ret = static_cast<int>(fwrite(s_line, 1, n, stdout));
    } else {
        ret = s_orig(fmt, ap);      // too long for the buffer: the serial port still gets all of it
    }
    if (n > 0) {
        const bool cut = n >= static_cast<int>(sizeof(s_line));
        size_t len = cut ? sizeof(s_line) - 1 : static_cast<size_t>(n);
        if (cut) {
            s_line[len - 1] = '\n';
        }
        if (xRingbufferSend(s_ring, s_line, len, 0) != pdTRUE) {
            s_dropped++;
        }
    }
    xSemaphoreGive(s_fmt_mtx);
    return ret;
}

/* ---------------- files (writer task) ---------------- */

const char *reset_reason()
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "POWER GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU LOCKUP";
    default: return "unknown";
    }
}

bool clock_valid(time_t t)
{
    return t > 1700000000;      // set by SNTP or the 4G network, else 1970
}

void write_raw(const char *s, size_t len)
{
    if (s_file && fwrite(s, 1, len, s_file) == len) {
        s_file_size += static_cast<long>(len);
    } else if (s_file) {
        fclose(s_file);
        s_file = nullptr;
        s_retry_at_us = esp_timer_get_time() + RETRY_MS * 1000LL;
    }
}

void write_note(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        write_raw(buf, n < static_cast<int>(sizeof(buf)) ? n : sizeof(buf) - 1);
    }
}

void write_clock()
{
    const time_t now = time(nullptr);
    if (!clock_valid(now)) {
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    write_note("--- clock %s, up %lld s\n", when, esp_timer_get_time() / 1000000);
    s_last_clock = now;
}

/* Lowest and highest log_NNNN.txt numbers, and how many there are */
int scan_dir(int *lowest, int *highest)
{
    *lowest = 0;
    *highest = 0;
    int count = 0;
    DIR *d = opendir(LOG_DIR);
    if (!d) {
        return 0;
    }
    while (struct dirent *e = readdir(d)) {
        int no;
        char tail[8];
        /* FAT short names may come back in upper case */
        if (sscanf(e->d_name, "%*[lL]%*[oO]%*[gG]_%d.%3s", &no, tail) == 2 && no > 0) {
            count++;
            if (*lowest == 0 || no < *lowest) {
                *lowest = no;
            }
            if (no > *highest) {
                *highest = no;
            }
        }
    }
    closedir(d);
    return count;
}

void file_path(char *buf, size_t len, int no)
{
    snprintf(buf, len, "%s/log_%04d.txt", LOG_DIR, no);
}

/* Open the file of this boot (again after a pause), or the next one when `next` */
bool open_file(bool next)
{
    char path[64];
    file_path(path, sizeof(path), 1);
    if (phone_sd_make_dirs(path) != 0) {
        return false;
    }
    const bool fresh = s_file_no == 0 || next;
    if (fresh) {
        int lowest, highest;
        int count = scan_dir(&lowest, &highest);
        /* the newest KEEP_FILES stay, counting the new one */
        for (int no = lowest; count >= KEEP_FILES && no > 0 && no <= highest; no++) {
            file_path(path, sizeof(path), no);
            if (unlink(path) == 0) {
                count--;
            }
        }
        s_file_no = highest >= 9999 ? 1 : highest + 1;
    }
    file_path(path, sizeof(path), s_file_no);
    s_file = fopen(path, "a");
    if (!s_file) {
        return false;
    }
    fseek(s_file, 0, SEEK_END);
    s_file_size = ftell(s_file);
    if (fresh) {
        const esp_app_desc_t *app = esp_app_get_description();
        write_note("=== %s %s, %s, reset: %s, up %lld ms\n", app->project_name, app->version, path,
                   reset_reason(), esp_timer_get_time() / 1000);
        write_clock();
    } else {
        write_note("--- card back, up %lld s\n", esp_timer_get_time() / 1000000);
    }
    s_last_sync_us = esp_timer_get_time();
    return s_file != nullptr;
}

void close_file()
{
    if (s_file) {
        fflush(s_file);
        fsync(fileno(s_file));
        fclose(s_file);
        s_file = nullptr;
    }
}

bool card_ready()
{
    return !s_paused && esp_timer_get_time() >= s_retry_at_us && phone_storage_get(nullptr, nullptr) == PHONE_STORAGE_MOUNTED;
}

void writer_task(void *)
{
    for (;;) {
        xSemaphoreTake(s_file_mtx, portMAX_DELAY);
        if (!s_file && s_on && card_ready()) {
            if (!open_file(false)) {
                s_retry_at_us = esp_timer_get_time() + RETRY_MS * 1000LL;
            }
        }
        const bool have_file = s_file != nullptr;
        xSemaphoreGive(s_file_mtx);

        if (!have_file) {
            if (!s_on) {
                /* switched off without a file: what is left goes away */
                size_t n;
                while (void *old = xRingbufferReceiveUpTo(s_ring, &n, 0, CHUNK_MAX)) {
                    vRingbufferReturnItem(s_ring, old);
                }
            }
            /* else keep the lines in the ring until the card is there */
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        size_t len = 0;
        void *data = xRingbufferReceiveUpTo(s_ring, &len, pdMS_TO_TICKS(1000), CHUNK_MAX);

        xSemaphoreTake(s_file_mtx, portMAX_DELAY);
        if (s_file) {
            const uint32_t dropped = s_dropped.exchange(0);
            if (dropped) {
                write_note("--- %u log lines not saved (buffer full or busy)\n", static_cast<unsigned>(dropped));
            }
            if (data) {
                write_raw(static_cast<const char *>(data), len);
            }
            const time_t now = time(nullptr);
            if (clock_valid(now) && (!clock_valid(s_last_clock) || now - s_last_clock >= CLOCK_EVERY_S)) {
                write_clock();
            }
            const int64_t t = esp_timer_get_time();
            if (s_file && t - s_last_sync_us >= SYNC_MS * 1000LL) {
                s_last_sync_us = t;
                if (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0) {
                    fclose(s_file);
                    s_file = nullptr;
                    s_retry_at_us = t + RETRY_MS * 1000LL;
                }
            }
            if (s_file && s_file_size >= FILE_MAX) {
                close_file();
                open_file(true);
            }
            /* switched off: close once the ring is empty */
            if (!s_on && !data) {
                close_file();
            }
        } else if (data) {
            s_dropped++;    // the card went away under this chunk
        }
        /* closed by phone_sdlog_storage_pause while this task was writing */
        if (s_paused && s_file) {
            fclose(s_file);
            s_file = nullptr;
        }
        xSemaphoreGive(s_file_mtx);
        if (data) {
            vRingbufferReturnItem(s_ring, data);
        }
    }
}

bool start()
{
    if (s_task) {
        return true;
    }
    s_ring = xRingbufferCreateWithCaps(RING_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    s_fmt_mtx = xSemaphoreCreateMutex();
    s_file_mtx = xSemaphoreCreateMutex();
    if (!s_ring || !s_fmt_mtx || !s_file_mtx ||
            xTaskCreate(writer_task, "sdlog", 4096, nullptr, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        return false;
    }
    return true;
}

void apply(bool on)
{
    if (on) {
        if (!start()) {
            return;
        }
        s_on = true;
        if (!s_hooked) {
            s_orig = esp_log_set_vprintf(log_hook);
            s_hooked = true;
        }
        ESP_LOGI(TAG, "log to %s", LOG_DIR);
    } else if (s_hooked) {
        ESP_LOGI(TAG, "log to the card off");
        s_on = false;
        esp_log_set_vprintf(s_orig);
        s_hooked = false;
    }
}

} // namespace

extern "C" void phone_sdlog_init(void)
{
    apply(phone_pref_get(PREF_KEY, 0) != 0);
}

extern "C" void phone_sdlog_set(bool on)
{
    phone_pref_set(PREF_KEY, on ? 1 : 0);
    apply(on);
}

extern "C" bool phone_sdlog_is_on(void)
{
    return s_on;
}

extern "C" void phone_sdlog_storage_pause(void)
{
    if (!s_task) {
        return;
    }
    s_paused = true;
    /* a write stuck on the card: go on, the writer closes the file when it returns */
    if (xSemaphoreTake(s_file_mtx, pdMS_TO_TICKS(2000)) == pdTRUE) {
        close_file();
        xSemaphoreGive(s_file_mtx);
    }
}

extern "C" void phone_sdlog_storage_resume(void)
{
    s_paused = false;
}
