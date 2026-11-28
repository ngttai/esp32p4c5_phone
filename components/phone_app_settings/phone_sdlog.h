#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Copy of the ESP_LOG output on the memory card, in /sdcard/system/logs/log_NNNN.txt: one file per boot
 * (a new one past 2 MB), the 10 newest kept; index.txt gives the date of each. Lines wait in PSRAM (128 KB)
 * until the card is mounted, so the boot is in the file too; lines that do not fit are counted and noted in the file. printf output and the
 * panic dump (written by the ROM) are not in it: the next boot notes the reset reason.
 */

/** Start logging when the setting is on; call early in app_main, after phone_locale_init(). */
void phone_sdlog_init(void);
/** Settings > Debug switch: saved, applies at once. */
void phone_sdlog_set(bool on);
bool phone_sdlog_is_on(void);

/** phone_storage: close the file before /sdcard is unmounted (USB drive, card recovery), open it again after. */
void phone_sdlog_storage_pause(void);
void phone_sdlog_storage_resume(void);

#ifdef __cplusplus
}
#endif
