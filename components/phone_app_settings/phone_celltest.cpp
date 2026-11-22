#include "phone_celltest.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "phone_cell.h"

#define TAG "CELL"

namespace {

constexpr uart_port_t PORT = static_cast<uart_port_t>(CONFIG_BSP_MODEM_UART_NUM);
constexpr size_t REPORT_SIZE = 12 * 1024;
constexpr int BAUD_DEFAULT = 115200;
constexpr int BAUD_TRIES = 300;             // "AT" round trips per baud rate
constexpr uint32_t REG_WAIT_MS = 90000;     // network registration

std::atomic<bool> s_running;
char *s_report;                              // PSRAM
size_t s_len;
SemaphoreHandle_t s_lock;                   // the report: written by the probe, read by the page

void add(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", line);
    const size_t n = strlen(line);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_report) {
        if (s_len + n + 2 > REPORT_SIZE) {          // keep the newest half
            const size_t keep = REPORT_SIZE / 2;
            memmove(s_report, s_report + s_len - keep, keep);
            s_len = keep;
        }
        memcpy(s_report + s_len, line, n);
        s_len += n;
        s_report[s_len++] = '\n';
        s_report[s_len] = '\0';
    }
    xSemaphoreGive(s_lock);
}

/* An answer on one line: CR LF become " | ", the final OK is kept */
void flatten(const char *in, char *out, size_t len)
{
    size_t o = 0;
    bool sep = false;
    for (const char *p = in; *p && o + 4 < len; p++) {
        if (*p == '\r' || *p == '\n') {
            sep = o > 0;
            continue;
        }
        if (sep) {
            out[o++] = ' ';
            out[o++] = '|';
            out[o++] = ' ';
            sep = false;
        }
        out[o++] = (*p >= 0x20 && *p < 0x7F) ? *p : '.';
    }
    out[o] = '\0';
}

esp_err_t at(const char *cmd, uint32_t timeout_ms = 2000, char *keep = nullptr, size_t keep_len = 0)
{
    char resp[512];
    const esp_err_t err = bsp_modem_at(cmd, resp, sizeof(resp), timeout_ms);
    char flat[400];
    flatten(resp, flat, sizeof(flat));
    add("> %s  -> %s%s", cmd, flat, err == ESP_ERR_TIMEOUT ? " [timeout]" : (err == ESP_FAIL ? " [error]" : ""));
    if (keep && keep_len) {
        strlcpy(keep, resp, keep_len);
    }
    return err;
}

/* Raw exchange: write, then read until `want` or `fail` shows up (or the time is out) */
bool raw(const char *cmd, const char *want, const char *fail, uint32_t timeout_ms, char *out, size_t out_len)
{
    uart_flush_input(PORT);
    if (cmd) {
        uart_write_bytes(PORT, cmd, strlen(cmd));
    }
    size_t len = 0;
    out[0] = '\0';
    const int64_t end = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (esp_timer_get_time() < end && len + 1 < out_len) {
        const int n = uart_read_bytes(PORT, out + len, out_len - 1 - len, pdMS_TO_TICKS(20));
        if (n > 0) {
            len += n;
            out[len] = '\0';
            if (want && strstr(out, want)) {
                return true;
            }
            if (fail && strstr(out, fail)) {
                return false;
            }
        }
    }
    return false;
}

/* How many of `tries` "AT" round trips come back with OK at the current UART rate */
int at_score(int tries)
{
    int ok = 0;
    char resp[64];
    for (int i = 0; i < tries; i++) {
        if (bsp_modem_at("AT", resp, sizeof(resp), 300) == ESP_OK) {
            ok++;
        }
    }
    return ok;
}

/* Find the rate the module answers at (it may keep a rate set by AT+IPR) */
int find_baud()
{
    static const int rates[] = {115200, 230400, 460800, 921600, 1500000, 3000000};
    for (int b : rates) {
        uart_set_baudrate(PORT, b);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (at_score(3) >= 2) {
            return b;
        }
    }
    uart_set_baudrate(PORT, BAUD_DEFAULT);
    return 0;
}

/* Move module and UART to `baud`; false (and back at 115200 if possible) when it does not hold */
bool switch_baud(int from, int baud)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+IPR=%d", baud);
    if (at(cmd, 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_set_baudrate(PORT, baud);
    vTaskDelay(pdMS_TO_TICKS(100));
    (void)from;
    return true;
}

void baud_test()
{
    add("--- baud rates (%d x \"AT\" each) ---", BAUD_TRIES);
    at("AT+IPR?");
    at("AT+IPR=?");
    const int64_t t0 = esp_timer_get_time();
    int score = at_score(BAUD_TRIES);
    add("115200: %d/%d OK, %lld ms", score, BAUD_TRIES, (esp_timer_get_time() - t0) / 1000);
    static const int rates[] = {230400, 460800, 921600};
    int current = BAUD_DEFAULT;
    for (int b : rates) {
        if (!switch_baud(current, b)) {
            add("%d: module refused AT+IPR", b);
            continue;
        }
        current = b;
        const int64_t t = esp_timer_get_time();
        score = at_score(BAUD_TRIES);
        add("%d: %d/%d OK, %lld ms", b, score, BAUD_TRIES, (esp_timer_get_time() - t) / 1000);
        if (score < BAUD_TRIES * 9 / 10) {
            add("%d does not hold: back to 115200", b);
            break;
        }
    }
    /* back to the BSP rate, whatever happened */
    char resp[64];
    for (int i = 0; i < 5 && current != BAUD_DEFAULT; i++) {
        if (bsp_modem_at("AT+IPR=115200", resp, sizeof(resp), 500) == ESP_OK) {
            break;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_set_baudrate(PORT, BAUD_DEFAULT);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (at_score(3) < 2) {
        const int found = find_baud();
        add("module lost at 115200, found at %d", found);
        if (found && found != BAUD_DEFAULT) {
            bsp_modem_at("AT+IPR=115200", resp, sizeof(resp), 500);
            vTaskDelay(pdMS_TO_TICKS(100));
            uart_set_baudrate(PORT, BAUD_DEFAULT);
        }
    }
    at("AT+IPR?");
}

bool wait_registered()
{
    const int64_t end = esp_timer_get_time() + static_cast<int64_t>(REG_WAIT_MS) * 1000;
    char resp[128];
    int last = -1;
    while (esp_timer_get_time() < end) {
        if (bsp_modem_at("AT+CEREG?", resp, sizeof(resp), 1000) == ESP_OK) {
            const char *p = strstr(resp, "+CEREG:");
            int n = 0, stat = -1;
            if (p && sscanf(p, "+CEREG: %d,%d", &n, &stat) == 2) {
                if (stat != last) {
                    add("CEREG stat %d after %lld s (1 home, 5 roaming, 2 searching, 3 denied)", stat,
                        (esp_timer_get_time() - (end - static_cast<int64_t>(REG_WAIT_MS) * 1000)) / 1000000);
                    last = stat;
                }
                if (stat == 1 || stat == 5) {
                    return true;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return false;
}

void dial_test()
{
    add("--- PPP dial ---");
    char buf[512];
    const int64_t t0 = esp_timer_get_time();
    const bool connect = raw("ATD*99#\r\n", "CONNECT", "NO CARRIER", 30000, buf, sizeof(buf));
    char flat[400];
    flatten(buf, flat, sizeof(flat));
    add("> ATD*99#  -> %s (%lld ms)", flat, (esp_timer_get_time() - t0) / 1000);
    if (!connect) {
        if (!strstr(buf, "ERROR") && !strstr(buf, "NO CARRIER")) {
            raw("ATH\r\n", "OK", "ERROR", 2000, buf, sizeof(buf));
        }
        return;
    }
    /* the module starts PPP (LCP frames, flag 0x7E) */
    uint8_t frames[256];
    int bytes = 0, flags = 0;
    const int64_t end = esp_timer_get_time() + 3000000;
    while (esp_timer_get_time() < end) {
        const int n = uart_read_bytes(PORT, frames, sizeof(frames), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            flags += frames[i] == 0x7E;
        }
        bytes += n > 0 ? n : 0;
    }
    add("PPP from the module in 3 s: %d bytes, %d frame flags", bytes, flags);
    /* hang up: guard time, +++, guard time, then ATH */
    vTaskDelay(pdMS_TO_TICKS(1200));
    const bool escaped = raw("+++", "OK", nullptr, 2500, buf, sizeof(buf));
    add("+++ -> %s", escaped ? "OK (command mode)" : "no OK");
    raw("ATH\r\n", "OK", "ERROR", 3000, buf, sizeof(buf));
    flatten(buf, flat, sizeof(flat));
    add("> ATH  -> %s", flat);
}

/* Bytes as hex, for the CMUX frames */
void add_hex(const char *what, const uint8_t *b, int n)
{
    char line[300];
    int o = snprintf(line, sizeof(line), "%s (%d bytes):", what, n);
    for (int i = 0; i < n && o + 4 < static_cast<int>(sizeof(line)); i++) {
        o += snprintf(line + o, sizeof(line) - o, " %02X", b[i]);
    }
    add("%s", line);
}

/* One way into CMUX: the AT command, then SABM on DLCI 0 (twice, 1 s apart), and what comes back.
 * Expected answer: UA on DLCI 0 = F9 03 73 01 D7 F9 */
bool cmux_try(const char *cmd)
{
    char buf[256];
    char at_cmd[64];
    snprintf(at_cmd, sizeof(at_cmd), "%s\r\n", cmd);
    const bool ok = raw(at_cmd, "OK", "ERROR", 2000, buf, sizeof(buf));
    char flat[200];
    flatten(buf, flat, sizeof(flat));
    add("> %s  -> %s", cmd, flat);
    if (!ok) {
        return false;
    }
    static const uint8_t sabm0[] = {0xF9, 0x03, 0x3F, 0x01, 0x1C, 0xF9};
    bool ua = false;
    for (int attempt = 0; attempt < 2 && !ua; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(attempt ? 1000 : 100));
        uart_flush_input(PORT);
        uart_write_bytes(PORT, sabm0, sizeof(sabm0));
        uint8_t in[128];
        int n = 0;
        const int64_t end = esp_timer_get_time() + 1500000;
        while (esp_timer_get_time() < end && n < static_cast<int>(sizeof(in))) {
            const int r = uart_read_bytes(PORT, in + n, sizeof(in) - n, pdMS_TO_TICKS(50));
            n += r > 0 ? r : 0;
        }
        char what[48];
        snprintf(what, sizeof(what), "SABM DLCI0 #%d answer", attempt + 1);
        add_hex(what, in, n);
        for (int i = 0; i + 2 < n; i++) {
            if (in[i] == 0xF9 && in[i + 1] == 0x03 && (in[i + 2] & 0xEF) == 0x63) {
                ua = true;
            }
        }
    }
    add("%s: %s", cmd, ua ? "UA received, CMUX works" : "no UA");
    return ua;
}

void cmux_test()
{
    add("--- CMUX (raw frames) ---");
    static const char *const ways[] = {"AT+CMUX=0", "AT+CMUX=0,0,5,127"};
    for (const char *way : ways) {
        /* a fresh module for each way: once in CMUX it no longer takes AT */
        bsp_modem_deinit();
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (bsp_modem_init() != ESP_OK) {
            add("no answer after power cycle");
            return;
        }
        if (cmux_try(way)) {
            return;
        }
    }
}

void test_task(void *)
{
    add("=== 4G probe ===");
    const int64_t t0 = esp_timer_get_time();
    if (bsp_modem_init() != ESP_OK) {
        /* the BSP waits at 115200 only: after 4G the module may still be at 921600 (AT+IPR is kept) */
        int found = 0;
        if (uart_is_driver_installed(PORT) && bsp_power_set(BSP_POWER_MODEM, true) == ESP_OK) {
            const int64_t end = esp_timer_get_time() + 12 * 1000 * 1000LL;
            while (!found && esp_timer_get_time() < end) {
                found = find_baud();
            }
        }
        if (!found) {
            add("No answer to AT within 15 s at any rate. The module runs from the battery: is it in?");
            bsp_modem_deinit();         /* frees the UART: the 4G service needs it */
            s_running = false;
            vTaskDelete(nullptr);
            return;
        }
        add("no answer at 115200, answers at %d after %lld ms", found, (esp_timer_get_time() - t0) / 1000);
        char resp[64];
        bsp_modem_at("ATE0", resp, sizeof(resp), 500);
    } else {
        add("answers AT after %lld ms", (esp_timer_get_time() - t0) / 1000);
    }
    /* it may keep a rate from a previous probe */
    if (at_score(3) < 2) {
        add("no answer at 115200, found at %d", find_baud());
    }

    add("--- identity ---");
    at("ATI");
    at("AT+CGMI");
    at("AT+CGMM");
    at("AT+CGMR");
    at("AT+CGSN");
    at("AT+CMUX=?");
    at("AT+CMUX?");
    at("AT&D1");
    at("AT+CFUN?");

    add("--- SIM ---");
    char resp[256];
    const bool sim = at("AT+CPIN?", 5000, resp, sizeof(resp)) == ESP_OK && strstr(resp, "READY");
    at("AT+CIMI");
    at("AT+CCID");
    at("AT+ICCID");

    baud_test();

    if (sim) {
        add("--- network ---");
        const bool reg = wait_registered();
        at("AT+CSQ");
        at("AT+CESQ");
        at("AT+COPS=3,0");
        at("AT+COPS?", 5000);
        at("AT+COPS=3,2");
        at("AT+COPS?", 5000);
        at("AT+CGDCONT?");
        at("AT+CGATT?", 5000);
        at("AT+CGACT?", 5000);
        at("AT+CGCONTRDP", 5000);
        at("AT+CGPADDR");
        if (reg) {
            dial_test();
            at("AT", 1000);
        } else {
            add("not registered within %u s: no dial", static_cast<unsigned>(REG_WAIT_MS / 1000));
        }
    } else {
        add("no SIM ready: network steps skipped");
    }

    cmux_test();

    bsp_modem_deinit();
    add("=== done in %lld s, modem off ===", (esp_timer_get_time() - t0) / 1000000);
    s_running = false;
    vTaskDelete(nullptr);
}

} // namespace

extern "C" bool phone_celltest_start(void)
{
    if (phone_cell_busy() || s_running.exchange(true)) {
        return false;           /* the 4G service owns the modem while 4G is on */
    }
    if (!s_report) {
        s_report = static_cast<char *>(heap_caps_malloc(REPORT_SIZE, MALLOC_CAP_SPIRAM));
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        s_running = false;
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_len = 0;
    if (s_report) {
        s_report[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    /* internal stack: the UART driver and the AT parsing are short; no flash access */
    if (xTaskCreate(test_task, "celltest", 6144, nullptr, 4, nullptr) != pdPASS) {
        s_running = false;
        return false;
    }
    return true;
}

extern "C" bool phone_celltest_running(void)
{
    return s_running;
}

extern "C" void phone_celltest_text(char *buf, size_t len)
{
    if (!buf || !len) {
        return;
    }
    buf[0] = '\0';
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    if (s_report && s_len) {
        const size_t from = s_len + 1 > len ? s_len + 1 - len : 0;
        memcpy(buf, s_report + from, s_len - from);
        buf[s_len - from] = '\0';
    }
    xSemaphoreGive(s_lock);
}
