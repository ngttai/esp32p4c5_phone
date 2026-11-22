#include "phone_cell.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "phone_locale.h"
#include "phone_net.h"

#define TAG "CELL"
#define T(vi, en) phone_tr(vi, en)

namespace {

constexpr uint32_t POLL_SEARCH_MS = 5000;       // until registered
constexpr uint32_t POLL_MS = 15000;             // registered
constexpr uint32_t RETRY_NO_ANSWER_MS = 60000;  // power again after a module that did not answer
constexpr uint32_t BOOT_MS = 15000;             // first "AT" answer after power-on (4.7 s measured)
constexpr uint32_t DATA_RETRY_MS = 20000;       // after a PPP that did not come up
constexpr uint32_t PPP_UP_MS = 30000;           // IP from PPP after the dial
constexpr uint32_t WIFI_LOST_MS = 15000;        // Wi-Fi down this long (a reason-7 drop is back in a few s) -> 4G
constexpr uint32_t WIFI_BACK_MS = 30000;        // Wi-Fi back this long -> 4G to standby
constexpr int BAUD_BOOT = 115200;
constexpr int BAUD_FAST = 921600;               // stage 0: 300/300 "AT" through the level shifter

TaskHandle_t s_task;
std::atomic<bool> s_want;
std::atomic<bool> s_want_data;
std::atomic<bool> s_powered;
SemaphoreHandle_t s_lock;
phone_cell_info_t s_info;
phone_cell_state_t s_logged_state = PHONE_CELL_OFF;
int s_logged_bars = -1;
phone_cell_data_t s_logged_data = PHONE_CELL_DATA_OFF;

esp_netif_t *s_ppp;                 // created once
esp_modem_dce_t *s_dce;             // while the modem is powered
esp_modem_dce_mode_t s_mode = ESP_MODEM_MODE_COMMAND;
int s_baud = BAUD_BOOT;
std::atomic<bool> s_ppp_up;
std::atomic<bool> s_ppp_lost;
int64_t s_data_retry_at;
int s_wifi_seen = -1;                           // last sampled Wi-Fi state (-1: not yet)
int64_t s_wifi_since;                           // when it last changed

const uart_port_t PORT = static_cast<uart_port_t>(CONFIG_BSP_MODEM_UART_NUM);

/* ---- info ---- */

void log_change()
{
    phone_cell_info_t i;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    i = s_info;
    xSemaphoreGive(s_lock);
    if (i.state == s_logged_state && i.bars == s_logged_bars && i.data == s_logged_data) {
        return;
    }
    s_logged_state = i.state;
    s_logged_bars = i.bars;
    s_logged_data = i.data;
    if (i.state == PHONE_CELL_REGISTERED) {
        ESP_LOGI(TAG, "registered %s%s %s, %d dBm, %d/4, APN %s, IP %s; data %d %s %s", i.oper, i.roaming ? " (roaming)" : "",
                 i.act, i.rssi_dbm, i.bars, i.apn, i.ip, static_cast<int>(i.data), i.data_ip, i.link);
    } else {
        ESP_LOGI(TAG, "state %d, data %d", static_cast<int>(i.state), static_cast<int>(i.data));
    }
}

void set_state(phone_cell_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.state = st;
    if (st != PHONE_CELL_REGISTERED) {
        s_info.bars = 0;
        s_info.roaming = false;
    }
    xSemaphoreGive(s_lock);
}

void set_data(phone_cell_data_t d, const char *ip = nullptr)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.data = d;
    strlcpy(s_info.data_ip, ip ? ip : "", sizeof(s_info.data_ip));
    if (d == PHONE_CELL_DATA_OFF || d == PHONE_CELL_DATA_WAITING) {
        s_info.link[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

/* ---- AT through esp_modem ---- */

/* An AT command in command or CMUX mode; the answer as esp_modem returns it (lines without the OK) */
bool at(const char *cmd, char *resp, size_t len, uint32_t timeout_ms = 2000)
{
    static char out[CONFIG_ESP_MODEM_C_API_STR_MAX + 1];   // only the phone_cell task talks AT
    out[0] = '\0';
    resp[0] = '\0';
    if (!s_dce || s_mode == ESP_MODEM_MODE_DATA) {
        return false;           /* the UART carries PPP only */
    }
    const bool ok = esp_modem_at(s_dce, cmd, out, timeout_ms) == ESP_OK;
    strlcpy(resp, out, len);
    return ok;
}

/* The text after "<tag>" on its line (tag like "+CEREG:"), or the first non-empty line when tag is null */
bool field(const char *resp, const char *tag, char *out, size_t len)
{
    const char *p = resp;
    if (tag) {
        p = strstr(resp, tag);
        if (!p) {
            return false;
        }
        p += strlen(tag);
    } else {
        while (*p == '\r' || *p == '\n') {
            p++;
        }
    }
    while (*p == ' ') {
        p++;
    }
    size_t n = 0;
    while (p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < len) {
        n++;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return n > 0 && strcmp(out, "OK") != 0;
}

/* The n-th (0 based) comma separated item of a line, without quotes */
void item(const char *line, int n, char *out, size_t len)
{
    out[0] = '\0';
    const char *p = line;
    for (int i = 0; i < n && p; i++) {
        p = strchr(p, ',');
        if (p) {
            p++;
        }
    }
    if (!p) {
        return;
    }
    size_t o = 0;
    for (; *p && *p != ',' && o + 1 < len; p++) {
        if (*p != '"') {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

const char *operator_name(const char *mccmnc)
{
    static const struct {
        const char *code, *name;
    } table[] = {
        {"45204", "Viettel"}, {"45202", "Vinaphone"}, {"45201", "MobiFone"}, {"45205", "Vietnamobile"}, {"45207", "Gmobile"},
    };
    for (const auto &t : table) {
        if (strcmp(mccmnc, t.code) == 0) {
            return t.name;
        }
    }
    return mccmnc;
}

/* APN by operator when the network gives none (values from the usual settings, not checked on every SIM) */
const char *operator_apn(const char *mccmnc)
{
    static const struct {
        const char *code, *apn;
    } table[] = {
        {"45204", "v-internet"}, {"45202", "m3-world"}, {"45201", "m-wap"}, {"45205", "internet"},
    };
    for (const auto &t : table) {
        if (strcmp(mccmnc, t.code) == 0) {
            return t.apn;
        }
    }
    return "";
}

const char *act_name(int act)
{
    switch (act) {
    case 0: return "GSM";
    case 2: return "UMTS";
    case 7: return "LTE";
    case 9: return "NB-IoT";
    default: return "";
    }
}

/* ---- modem power and link ---- */

void on_ip_event(void *, esp_event_base_t, int32_t id, void *data)
{
    if (id == IP_EVENT_PPP_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        char ip[40];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "PPP up: %s", ip);
        set_data(PHONE_CELL_DATA_UP, ip);
        s_ppp_up = true;
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost its address");
        s_ppp_up = false;
        s_ppp_lost = true;
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void on_terminal_error(esp_modem_terminal_error_t err)
{
    ESP_LOGW(TAG, "terminal error %d", static_cast<int>(err));
}

bool sync_at(int baud, int tries)
{
    uart_set_baudrate(PORT, baud);
    for (int i = 0; i < tries; i++) {
        if (esp_modem_sync(s_dce) == ESP_OK) {
            s_baud = baud;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/* Power the module and open the esp_modem link in command mode; false when it never answers */
bool power_on()
{
    if (!s_ppp) {
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
        s_ppp = esp_netif_new(&cfg);
        esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, on_ip_event, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, on_ip_event, nullptr);
    }
    if (uart_is_driver_installed(PORT)) {
        /* left by a Debug > 4G probe that stopped early: esp_modem installs its own driver */
        ESP_LOGW(TAG, "UART %d was still installed: freed", PORT);
        uart_driver_delete(PORT);
    }
    esp_modem_dte_config_t dte = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte.dte_buffer_size = 2048;
    dte.task_stack_size = 4096;
    dte.task_priority = 6;
    dte.uart_config.port_num = PORT;
    dte.uart_config.baud_rate = BAUD_BOOT;
    dte.uart_config.tx_io_num = BSP_MODEM_TX;
    dte.uart_config.rx_io_num = BSP_MODEM_RX;
    dte.uart_config.rts_io_num = UART_PIN_NO_CHANGE;
    dte.uart_config.cts_io_num = UART_PIN_NO_CHANGE;
    dte.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte.uart_config.rx_buffer_size = 8192;          /* no RTS/CTS: room for a burst while the task is late */
    dte.uart_config.tx_buffer_size = 2048;
    esp_modem_dce_config_t dce = ESP_MODEM_DCE_DEFAULT_CONFIG("");
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_GENERIC, &dte, &dce, s_ppp);
    if (!s_dce) {
        ESP_LOGE(TAG, "esp_modem: no DCE");
        return false;
    }
    esp_modem_set_error_cb(s_dce, on_terminal_error);
    s_mode = ESP_MODEM_MODE_COMMAND;
    bsp_power_set(BSP_POWER_MODEM, true);
    /* the module answers ~5 s after power-on, at 115200 or at a rate kept from before */
    const int64_t end = esp_timer_get_time() + static_cast<int64_t>(BOOT_MS) * 1000;
    bool ok = false;
    while (!ok && esp_timer_get_time() < end && s_want) {
        ok = sync_at(BAUD_BOOT, 3) || sync_at(BAUD_FAST, 2);
    }
    if (!ok) {
        esp_modem_destroy(s_dce);
        s_dce = nullptr;
        bsp_power_set(BSP_POWER_MODEM, false);
        return false;
    }
    esp_modem_set_echo(s_dce, false);
    /* faster UART for PPP: the module switches after its OK, then the P4 side follows */
    if (s_baud != BAUD_FAST) {
        if (esp_modem_set_baud(s_dce, BAUD_FAST) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (!sync_at(BAUD_FAST, 5)) {
                ESP_LOGW(TAG, "no answer at %d, back to %d", BAUD_FAST, BAUD_BOOT);
                sync_at(BAUD_BOOT, 5);
            }
        }
    }
    ESP_LOGI(TAG, "modem link at %d baud", s_baud);
    return true;
}

void data_down()
{
    if (s_dce && s_mode != ESP_MODEM_MODE_COMMAND) {
        if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND) != ESP_OK) {
            ESP_LOGW(TAG, "back to command mode failed");
        }
        s_mode = ESP_MODEM_MODE_COMMAND;
        ESP_LOGI(TAG, "data off");
    }
    s_ppp_up = false;
    s_ppp_lost = false;
}

void power_off()
{
    data_down();
    if (s_dce) {
        char resp[64];
        if (s_baud != BAUD_BOOT) {
            /* the BSP probe and the Debug page talk at 115200 */
            const bool ok = at("AT+IPR=115200", resp, sizeof(resp));
            ESP_LOGI(TAG, "back to 115200: %s", ok ? "ok" : "no answer");
        }
        esp_modem_destroy(s_dce);
        s_dce = nullptr;
    }
    if (s_powered) {
        bsp_power_set(BSP_POWER_MODEM, false);
        s_powered = false;
        ESP_LOGI(TAG, "modem off");
    }
    s_baud = BAUD_BOOT;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info = {};
    xSemaphoreGive(s_lock);
    s_logged_state = PHONE_CELL_OFF;
    s_logged_bars = -1;
    s_logged_data = PHONE_CELL_DATA_OFF;
}

/* Dial: CMUX first (AT keeps working beside PPP), else plain data mode */
void data_up()
{
    phone_cell_info_t i;
    phone_cell_get(&i);
    const char *apn = i.apn[0] ? i.apn : operator_apn(i.mccmnc);
    set_data(PHONE_CELL_DATA_CONNECTING);
    esp_modem_set_apn(s_dce, apn);
    ESP_LOGI(TAG, "data: APN \"%s\", CMUX first", apn);
    s_ppp_up = false;
    s_ppp_lost = false;
    esp_modem_dce_mode_t mode = ESP_MODEM_MODE_CMUX;
    if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX) != ESP_OK) {
        ESP_LOGW(TAG, "CMUX refused: plain data mode");
        esp_modem_sync(s_dce);
        mode = ESP_MODEM_MODE_DATA;
        if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA) != ESP_OK) {
            ESP_LOGE(TAG, "data mode failed");
            set_data(PHONE_CELL_DATA_FAILED);
            s_data_retry_at = esp_timer_get_time() + static_cast<int64_t>(DATA_RETRY_MS) * 1000;
            return;
        }
    }
    s_mode = mode;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_info.link, sizeof(s_info.link), "%s %d", mode == ESP_MODEM_MODE_CMUX ? "CMUX" : "PPP", s_baud);
    xSemaphoreGive(s_lock);
    /* the IP comes with IP_EVENT_PPP_GOT_IP */
    const int64_t end = esp_timer_get_time() + static_cast<int64_t>(PPP_UP_MS) * 1000;
    while (!s_ppp_up && esp_timer_get_time() < end && s_want && s_want_data) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
    }
    if (!s_ppp_up) {
        ESP_LOGW(TAG, "no IP from PPP");
        data_down();
        set_data(PHONE_CELL_DATA_FAILED);
        s_data_retry_at = esp_timer_get_time() + static_cast<int64_t>(DATA_RETRY_MS) * 1000;
    }
}

/* ---- polling ---- */

void read_identity()
{
    char resp[160], v[64];
    phone_cell_info_t id = {};
    if (at("AT+CGMM", resp, sizeof(resp)) && field(resp, nullptr, v, sizeof(v))) {
        strlcpy(id.model, v, sizeof(id.model));
    }
    if (at("AT+CGMR", resp, sizeof(resp)) && field(resp, nullptr, v, sizeof(v))) {
        strlcpy(id.firmware, v, sizeof(id.firmware));
    }
    if (at("AT+CGSN", resp, sizeof(resp)) && field(resp, nullptr, v, sizeof(v))) {
        strlcpy(id.imei, v, sizeof(id.imei));
    }
    at("AT+COPS=3,2", resp, sizeof(resp));      /* operator as MCC-MNC: the text form comes back garbled */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_info.model, id.model, sizeof(id.model));
    memcpy(s_info.firmware, id.firmware, sizeof(id.firmware));
    memcpy(s_info.imei, id.imei, sizeof(id.imei));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s %s, IMEI %s", id.model, id.firmware, id.imei);
}

/* SIM: true when ready; reads ICCID and IMSI once */
bool poll_sim()
{
    char resp[128], v[64];
    if (!at("AT+CPIN?", resp, sizeof(resp), 5000) || !field(resp, "+CPIN:", v, sizeof(v))) {
        set_state(PHONE_CELL_NO_SIM);
        return false;
    }
    if (strcmp(v, "READY") != 0) {
        set_state(PHONE_CELL_SIM_LOCKED);
        return false;
    }
    if (!s_info.iccid[0]) {
        char iccid[24] = "", imsi[20] = "";
        if (at("AT+ICCID", resp, sizeof(resp)) && field(resp, "+ICCID:", v, sizeof(v))) {
            strlcpy(iccid, v, sizeof(iccid));
        }
        if (at("AT+CIMI", resp, sizeof(resp)) && field(resp, nullptr, v, sizeof(v))) {
            strlcpy(imsi, v, sizeof(imsi));
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_info.iccid, iccid, sizeof(s_info.iccid));
        strlcpy(s_info.imsi, imsi, sizeof(s_info.imsi));
        xSemaphoreGive(s_lock);
    }
    return true;
}

/* The address of AT+CGCONTRDP (3GPP 27.007): dot-separated numbers, 4 (IPv4), 8 (IPv4 + mask), 16 (IPv6) or 32
 * (IPv6 + mask). An IPV4V6 context gives one line per family: the IPv4 one is shown, else the IPv6 one in hex. */
bool contrdp_ip(const char *resp, char *apn, size_t apn_len, char *out, size_t len)
{
    bool v6 = false;
    out[0] = '\0';
    for (const char *p = strstr(resp, "+CGCONTRDP:"); p; p = strstr(p + 1, "+CGCONTRDP:")) {
        char line[320], addr[200];
        field(p, "+CGCONTRDP:", line, sizeof(line));
        if (!apn[0]) {
            item(line, 2, apn, apn_len);
        }
        item(line, 3, addr, sizeof(addr));
        int b[32], n = 0;
        for (const char *q = addr; *q && n < 32;) {
            b[n++] = atoi(q);
            q = strchr(q, '.');
            q = q ? q + 1 : "";
        }
        if (n == 4 || n == 8) {
            snprintf(out, len, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
            return true;
        }
        if ((n == 16 || n == 32) && !v6) {
            uint16_t g[8];
            for (int i = 0; i < 8; i++) {
                g[i] = static_cast<uint16_t>((b[2 * i] << 8) | b[2 * i + 1]);
            }
            int zs = -1, zl = 0;                    /* the longest run of zero groups becomes "::" */
            for (int i = 0; i < 8;) {
                int j = i;
                while (j < 8 && g[j] == 0) {
                    j++;
                }
                if (j - i > zl && j - i >= 2) {
                    zs = i;
                    zl = j - i;
                }
                i = j > i ? j : i + 1;
            }
            size_t o = 0;
            for (int i = 0; i < 8 && o < len; i++) {
                if (i == zs) {
                    o += snprintf(out + o, len - o, "::");
                    i += zl - 1;
                    continue;
                }
                o += snprintf(out + o, len - o, "%s%x", (i && i != zs + zl) ? ":" : "", g[i]);
            }
            v6 = true;
        }
    }
    return v6;
}

void poll_network()
{
    char resp[256], line[160], v[64];
    int stat = -1;
    if (at("AT+CEREG?", resp, sizeof(resp)) && field(resp, "+CEREG:", line, sizeof(line))) {
        item(line, 1, v, sizeof(v));
        stat = atoi(v);
    }
    const bool registered = stat == 1 || stat == 5;
    /* signal: RSRP of LTE (AT+CESQ item 5, dBm = value - 141), else CSQ (dBm = -113 + 2 * value) */
    int dbm = 0;
    if (at("AT+CESQ", resp, sizeof(resp)) && field(resp, "+CESQ:", line, sizeof(line))) {
        item(line, 5, v, sizeof(v));
        const int rsrp = atoi(v);
        if (rsrp > 0 && rsrp <= 97) {
            dbm = rsrp - 141;
        }
    }
    if (!dbm && at("AT+CSQ", resp, sizeof(resp)) && field(resp, "+CSQ:", line, sizeof(line))) {
        const int csq = atoi(line);
        if (csq > 0 && csq < 32) {
            dbm = -113 + 2 * csq;
        }
    }
    int bars = 0;
    if (registered && dbm) {
        bars = dbm >= -90 ? 4 : dbm >= -100 ? 3 : dbm >= -110 ? 2 : 1;
    }
    char mccmnc[8] = "", act[8] = "", apn[64] = "", ip[40] = "";
    if (registered) {
        if (at("AT+COPS?", resp, sizeof(resp), 5000) && field(resp, "+COPS:", line, sizeof(line))) {
            item(line, 2, mccmnc, sizeof(mccmnc));
            item(line, 3, v, sizeof(v));
            strlcpy(act, v[0] ? act_name(atoi(v)) : "", sizeof(act));
        }
        static char ctx[768];                       /* IPV4V6: two lines with IPv6 addresses, masks and DNS */
        if (at("AT+CGCONTRDP", ctx, sizeof(ctx), 5000)) {
            contrdp_ip(ctx, apn, sizeof(apn), ip, sizeof(ip));
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.state = registered ? PHONE_CELL_REGISTERED : stat == 3 ? PHONE_CELL_DENIED : PHONE_CELL_SEARCHING;
    s_info.roaming = stat == 5;
    s_info.bars = bars;
    s_info.rssi_dbm = dbm;
    if (registered) {
        strlcpy(s_info.mccmnc, mccmnc, sizeof(s_info.mccmnc));
        strlcpy(s_info.oper, mccmnc[0] ? operator_name(mccmnc) : "", sizeof(s_info.oper));
        strlcpy(s_info.act, act, sizeof(s_info.act));
        if (apn[0]) {
            strlcpy(s_info.apn, apn, sizeof(s_info.apn));
        }
        if (ip[0]) {
            strlcpy(s_info.ip, ip, sizeof(s_info.ip));
        }
    }
    xSemaphoreGive(s_lock);
}

/* Wi-Fi first: does mobile data need to carry the traffic now? `wait_ms` shrinks to the next decision */
bool need_ppp(uint32_t *wait_ms)
{
    phone_net_status_t net = {};
    phone_net_get_status(&net);
    const int64_t now = esp_timer_get_time();
    const int up = net.wifi_connected ? 1 : 0;
    if (up != s_wifi_seen) {
        s_wifi_seen = up;
        s_wifi_since = now;
    }
    if (!up && !phone_net_is_enabled()) {
        return true;                                /* turned off by the user: no wait */
    }
    const bool ppp = s_mode != ESP_MODEM_MODE_COMMAND;
    const uint32_t delay = up ? WIFI_BACK_MS : WIFI_LOST_MS;
    const int64_t left_us = s_wifi_since + static_cast<int64_t>(delay) * 1000 - now;
    if (up ? !ppp : ppp) {
        return ppp;                                 /* already where it should be */
    }
    if (left_us > 0) {
        const uint32_t left = static_cast<uint32_t>(left_us / 1000) + 50;
        if (left < *wait_ms) {
            *wait_ms = left;
        }
        return ppp;                                 /* not long enough: keep as it is */
    }
    static int64_t logged_since = -1;
    if (logged_since != s_wifi_since) {
        logged_since = s_wifi_since;
        if (up) {
            ESP_LOGI(TAG, "Wi-Fi back for %u s: mobile data to standby", static_cast<unsigned>(delay / 1000));
        } else {
            ESP_LOGI(TAG, "Wi-Fi lost for %u s: mobile data takes over", static_cast<unsigned>(delay / 1000));
        }
    }
    return !up;
}

void cell_task(void *)
{
    for (;;) {
        if (!s_want) {
            power_off();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!s_dce) {
            set_state(PHONE_CELL_STARTING);
            s_powered = true;                       /* the probe must not start meanwhile */
            if (!power_on()) {
                s_powered = false;
                set_state(PHONE_CELL_NO_ANSWER);
                log_change();
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_NO_ANSWER_MS));
                continue;
            }
            read_identity();
        }

        /* mobile data: down when turned off, lost, or Wi-Fi is back; up when on, registered and Wi-Fi is gone */
        uint32_t route_wait = POLL_MS;
        const bool need = s_want_data && need_ppp(&route_wait);
        if (s_mode != ESP_MODEM_MODE_COMMAND && (!s_want_data || s_ppp_lost || !need)) {
            const bool lost = s_ppp_lost;
            data_down();
            if (!s_want_data) {
                set_data(PHONE_CELL_DATA_OFF);
            } else if (lost) {
                set_data(PHONE_CELL_DATA_FAILED);
                s_data_retry_at = esp_timer_get_time() + static_cast<int64_t>(DATA_RETRY_MS) * 1000;
            } else {
                set_data(PHONE_CELL_DATA_STANDBY);
            }
        }

        uint32_t wait = POLL_SEARCH_MS;
        if (s_mode == ESP_MODEM_MODE_DATA) {
            wait = POLL_MS;                         /* plain data mode: no AT, the last values stay */
        } else if (poll_sim()) {
            poll_network();
            if (s_info.state == PHONE_CELL_REGISTERED) {
                wait = POLL_MS;
                if (need && s_mode == ESP_MODEM_MODE_COMMAND && esp_timer_get_time() >= s_data_retry_at) {
                    data_up();
                } else if (!s_want_data) {
                    set_data(PHONE_CELL_DATA_OFF);
                } else if (!need && s_mode == ESP_MODEM_MODE_COMMAND) {
                    set_data(PHONE_CELL_DATA_STANDBY);
                }
            }
        }
        if (s_want_data && s_info.state != PHONE_CELL_REGISTERED && s_mode == ESP_MODEM_MODE_COMMAND) {
            set_data(PHONE_CELL_DATA_WAITING);
        }
        if (route_wait < wait) {
            wait = route_wait;                      /* the Wi-Fi delay ends before the next poll */
        }
        log_change();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
    }
}

} // namespace

extern "C" void phone_cell_start(void)
{
    if (s_task) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_want = phone_pref_get("cell", 0) != 0;
    s_want_data = phone_pref_get("cdata", 0) != 0;
    /* esp_modem and lwIP PPP: internal stack (the UART driver, no flash access) */
    if (!s_lock || xTaskCreate(cell_task, "phone_cell", 6144, nullptr, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "cannot start");
        s_task = nullptr;
    }
}

extern "C" void phone_cell_set_enabled(bool on)
{
    phone_pref_set("cell", on ? 1 : 0);
    s_want = on;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

extern "C" bool phone_cell_is_enabled(void)
{
    return s_want;
}

extern "C" void phone_cell_set_data(bool on)
{
    phone_pref_set("cdata", on ? 1 : 0);
    s_want_data = on;
    s_data_retry_at = 0;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

extern "C" bool phone_cell_data_enabled(void)
{
    return s_want_data;
}

extern "C" void phone_cell_wifi_changed(void)
{
    if (s_task && s_want && s_want_data) {
        xTaskNotifyGive(s_task);
    }
}

extern "C" bool phone_cell_busy(void)
{
    return s_want || s_powered;
}

extern "C" void phone_cell_get(phone_cell_info_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        *out = {};
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
    if (!s_want && out->state != PHONE_CELL_OFF && !s_powered) {
        out->state = PHONE_CELL_OFF;
    }
}

extern "C" const char *phone_cell_state_text(phone_cell_state_t state)
{
    switch (state) {
    case PHONE_CELL_OFF: return T("Tắt", "Off");
    case PHONE_CELL_STARTING: return T("Đang bật…", "Starting…");
    case PHONE_CELL_NO_ANSWER: return T("Không phản hồi (cần pin)", "No answer (needs the battery)");
    case PHONE_CELL_NO_SIM: return T("Không có SIM", "No SIM");
    case PHONE_CELL_SIM_LOCKED: return T("SIM bị khoá PIN", "SIM locked (PIN)");
    case PHONE_CELL_SEARCHING: return T("Đang tìm mạng…", "Searching…");
    case PHONE_CELL_DENIED: return T("Mạng từ chối", "Registration denied");
    case PHONE_CELL_REGISTERED: return T("Đã kết nối", "Registered");
    }
    return "";
}

extern "C" const char *phone_cell_data_text(phone_cell_data_t data)
{
    switch (data) {
    case PHONE_CELL_DATA_OFF: return T("Tắt", "Off");
    case PHONE_CELL_DATA_WAITING: return T("Chờ mạng…", "Waiting for the network…");
    case PHONE_CELL_DATA_CONNECTING: return T("Đang nối…", "Connecting…");
    case PHONE_CELL_DATA_UP: return T("Đã nối", "Connected");
    case PHONE_CELL_DATA_FAILED: return T("Lỗi, sẽ thử lại", "Failed, trying again");
    case PHONE_CELL_DATA_STANDBY: return T("Dự phòng (Wi-Fi)", "Standby (Wi-Fi)");
    }
    return "";
}
