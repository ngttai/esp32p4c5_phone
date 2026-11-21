#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 4G service (stage 1 of PLAN_4G.md): powers the ML307C modem when 4G is on (NVS key "cell"), reads its
 * identity once, then polls SIM, registration, signal, operator and the data context over AT. No data
 * yet (PPP is stage 2). One task owns the modem UART; the Debug > 4G probe only runs while 4G is off.
 */
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
} phone_cell_info_t;

/** At boot: turns 4G on if it was on */
void phone_cell_start(void);
void phone_cell_set_enabled(bool on);
bool phone_cell_is_enabled(void);
/** The modem is powered or being powered (the probe must not run) */
bool phone_cell_busy(void);
void phone_cell_get(phone_cell_info_t *out);
/** Short state text for a tile or a row, in the phone language */
const char *phone_cell_state_text(phone_cell_state_t state);

#ifdef __cplusplus
}
#endif
