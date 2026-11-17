# Local copy of `esp_driver_sdmmc` (ESP-IDF v6.1)

Copied from `$IDF_PATH/components/esp_driver_sdmmc` of ESP-IDF v6.1 (without `test_apps`). As a
project component it replaces the IDF one. One change, in `src/sd_trans_sdmmc.c`:

- `cmd_needs_auto_stop()` returns false for a command with the flag `SCF_NO_AUTO_STOP` (0x4000, a
  bit unused by `sd_protocol_types.h`).

Why: on this board the SDMMC host runs the microSD card (slot 0) and the ESP-Hosted SDIO link of the
ESP32-C5 (slot 1). During long multiple block writes (the USB drive of the Files app) the automatic
STOP_TRANSMISSION (CMD12) sometimes reaches the card with a CRC error; the card stays in the receive
state and the host locks up. The USB drive writes with SET_BLOCK_COUNT (CMD23) + WRITE_MULTIPLE_BLOCK
(CMD25) marked with the flag, so no CMD12 is sent. Every other user of the driver is unchanged.

When ESP-IDF is updated: copy the new component again and redo the change, or delete this folder.
