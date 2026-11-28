#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 4G service (stages 1-2 of PLAN_4G.md): powers the ML307C modem when 4G is on (NVS key "cell"), reads its
 * identity once, then polls SIM, registration, signal, operator and the data context over AT. With mobile
 * data on (NVS key "cdata") it brings up PPP through esp_modem, in CMUX when the module takes it (AT keeps
 * working beside the data), else in plain data mode (the last signal stays shown). Stage 3: Wi-Fi first,
 * PPP only after Wi-Fi has been lost for WIFI_LOST_MS (or turned off by the user), down again once Wi-Fi has
 * been back for WIFI_BACK_MS; the modem stays registered meanwhile ("standby"). The PPP interface also has a
 * lower route priority than Wi-Fi. One task owns the modem UART; the
 * Debug > 4G probe only runs while 4G is off.
 */
typedef enum {
    PHONE_CELL_DATA_OFF,
    PHONE_CELL_DATA_WAITING,    /**< on, but the modem is not registered yet */
    PHONE_CELL_DATA_CONNECTING,
    PHONE_CELL_DATA_UP,
    PHONE_CELL_DATA_FAILED,     /**< tries again in a moment */
    PHONE_CELL_DATA_STANDBY,    /**< on, but Wi-Fi carries the traffic: PPP comes up when Wi-Fi is lost */
} phone_cell_data_t;

typedef enum {
    PHONE_CELL_OFF,
    PHONE_CELL_STARTING,        /**< powered, waiting for the first "AT" answer (about 5 s) */
    PHONE_CELL_NO_ANSWER,       /**< no answer within 15 s: the module runs from the battery */
    PHONE_CELL_NO_SIM,
    PHONE_CELL_SIM_LOCKED,      /**< SIM PIN or PUK asked: not handled yet */
    PHONE_CELL_SEARCHING,
    PHONE_CELL_DENIED,
    PHONE_CELL_REGISTERED,      /**< home network or roaming */
} phone_cell_state_t;

typedef struct {
    phone_cell_state_t state;
    bool roaming;
    int bars;                   /**< 0..4 */
    int rssi_dbm;               /**< RSRP of LTE when known, else from CSQ; 0 when unknown */
    char oper[24];              /**< "Viettel", or the MCC-MNC when not in the table */
    char mccmnc[8];
    char act[8];                /**< "LTE", "GSM"... */
    char apn[64];               /**< of the default context, given by the network */
    char ip[40];
    char model[24];
    char firmware[40];
    char imei[24];
    char iccid[24];
    char imsi[20];
    phone_cell_data_t data;
    char data_ip[40];           /**< address of the PPP interface */
    char link[24];              /**< "CMUX 921600", "PPP 115200"... */
    /* serving cell, read only while phone_cell_set_detail(true) (and not in plain data mode) */
    int band;                   /**< LTE band (3 = 1800 MHz), 0 unknown */
    int earfcn;                 /**< -1 unknown */
    int pci;                    /**< -1 unknown */
    int rsrq_x10;               /**< RSRQ in tenths of dB (-75 = -7.5 dB) */
    int sinr_x10;               /**< SINR in tenths of dB */
} phone_cell_info_t;

/** At boot: turns 4G on if it was on */
void phone_cell_start(void);
void phone_cell_set_enabled(bool on);
bool phone_cell_is_enabled(void);
void phone_cell_set_data(bool on);
bool phone_cell_data_enabled(void);
const char *phone_cell_data_text(phone_cell_data_t data);
/** The modem is powered or being powered (the probe must not run) */
bool phone_cell_busy(void);
void phone_cell_get(phone_cell_info_t *out);
/** The Mobile network page is shown: the serving cell (band, EARFCN, PCI, RSRQ, SINR) is read with each poll */
void phone_cell_set_detail(bool on);
/** Screen on/off (phone_lock): with the screen off and no PPP running, the module may sleep and is not polled. */
void phone_cell_set_screen(bool on);
/** Wi-Fi went up or down (from phone_net): the route choice is checked at once */
void phone_cell_wifi_changed(void);
/** Bytes through PPP (4G only, PPP framing included), kept in NVS across restarts; `since` = time of the last
 * reset (0 when the clock was not set then) */
void phone_cell_data_usage(unsigned long long *rx, unsigned long long *tx, long long *since);
void phone_cell_data_usage_reset(void);
/** 4G is ready (registered on a network): only then the mobile data switch lights up and can be changed; before
 * that it shows off and the saved choice is kept */
bool phone_cell_ready(const phone_cell_info_t *info);
/** Short state text for a tile or a row, in the phone language */
const char *phone_cell_state_text(phone_cell_state_t state);
/**
 * What the 4G icon shows, as on the status bar: -1 hidden (4G off), 0 cross (no SIM, SIM locked, no answer),
 * 1 dim bars (no service yet), 2..5 one to four bars. *data_up: mobile data carries the traffic ("4G" label).
 */
int phone_cell_icon_level(bool *data_up);

#ifdef __cplusplus
}
#endif
