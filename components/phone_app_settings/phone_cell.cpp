#include "phone_cell.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "phone_locale.h"

#define TAG "CELL"
#define T(vi, en) phone_tr(vi, en)

namespace {

constexpr uint32_t POLL_SEARCH_MS = 5000;       // until registered
constexpr uint32_t POLL_MS = 15000;             // registered
constexpr uint32_t RETRY_NO_ANSWER_MS = 60000;  // power again after a module that did not answer

TaskHandle_t s_task;
std::atomic<bool> s_want;
std::atomic<bool> s_powered;
SemaphoreHandle_t s_lock;
phone_cell_info_t s_info;
phone_cell_state_t s_logged_state = PHONE_CELL_OFF;
int s_logged_bars = -1;

/* One log line when the state or the bars change (task only) */
void log_change()
{
    phone_cell_info_t i;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    i = s_info;
    xSemaphoreGive(s_lock);
    if (i.state == s_logged_state && i.bars == s_logged_bars) {
        return;
    }
    s_logged_state = i.state;
    s_logged_bars = i.bars;
    if (i.state == PHONE_CELL_REGISTERED) {
        ESP_LOGI(TAG, "registered %s%s %s, %d dBm, %d/4, APN %s, IP %s", i.oper, i.roaming ? " (roaming)" : "", i.act,
                 i.rssi_dbm, i.bars, i.apn, i.ip);
    } else {
        ESP_LOGI(TAG, "state %d", static_cast<int>(i.state));
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

/* One AT command; the answer without echo and final OK */
bool at(const char *cmd, char *resp, size_t len, uint32_t timeout_ms = 2000)
{
    return bsp_modem_at(cmd, resp, len, timeout_ms) == ESP_OK;
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
        if (at("AT+CGCONTRDP", resp, sizeof(resp), 5000) && field(resp, "+CGCONTRDP:", line, sizeof(line))) {
            item(line, 2, apn, sizeof(apn));
            item(line, 3, ip, sizeof(ip));
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
        strlcpy(s_info.apn, apn, sizeof(s_info.apn));
        strlcpy(s_info.ip, ip, sizeof(s_info.ip));
    }
    xSemaphoreGive(s_lock);
}

void power_off()
{
    if (s_powered) {
        bsp_modem_deinit();
        s_powered = false;
        ESP_LOGI(TAG, "modem off");
    }
    s_logged_state = PHONE_CELL_OFF;
    s_logged_bars = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info = {};
    xSemaphoreGive(s_lock);
}

void cell_task(void *)
{
    for (;;) {
        if (!s_want) {
            power_off();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!s_powered) {
            set_state(PHONE_CELL_STARTING);
            s_powered = true;                       /* the probe must not start meanwhile */
            if (bsp_modem_init() != ESP_OK) {
                s_powered = false;
                set_state(PHONE_CELL_NO_ANSWER);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_NO_ANSWER_MS));
                continue;
            }
            read_identity();
        }
        uint32_t wait = POLL_SEARCH_MS;
        if (poll_sim()) {
            poll_network();
            if (s_info.state == PHONE_CELL_REGISTERED) {
                wait = POLL_MS;
            }
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
    if (!s_lock || xTaskCreate(cell_task, "phone_cell", 4096, nullptr, 3, &s_task) != pdPASS) {
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
