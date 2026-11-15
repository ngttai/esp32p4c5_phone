#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHONE_STORAGE_DISABLED = 0,    /*!< CONFIG_PHONE_SD_CARD_MOUNT is off */
    PHONE_STORAGE_CHECKING,        /*!< Mount in progress */
    PHONE_STORAGE_MOUNTED,
    PHONE_STORAGE_NO_CARD,         /*!< No card, or a card that could not be mounted (never formatted) */
} phone_storage_state_t;

/** Mount the microSD card in the background (no card detect pin: checked at boot only). */
void phone_storage_start(void);
/** Current state; total and free are filled in bytes when mounted (either may be NULL). */
phone_storage_state_t phone_storage_get(uint64_t *total, uint64_t *free);

#ifdef __cplusplus
}
#endif
