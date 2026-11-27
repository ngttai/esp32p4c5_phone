# Architecture

Source layout, configuration and implementation notes.

## Layout

- `main/`: `main.cpp` starts the BSP display, the LVGL lock, the phone UI, the app registry and the services below.
- `components/phone_app_settings/`: the Settings app and the phone services:
  - `phone_app_settings.cpp`: Settings app pages.
  - `phone_net.cpp`: Wi-Fi service (saved networks, auto-reconnect, SNTP), `phone_net_online()` (Wi-Fi or 4G).
  - `phone_cell.cpp`: 4G service (module power, SIM, registration, signal, operator, PPP, Wi-Fi first, data counter); `phone_celltest.cpp`: the 4G module test of the Debug page.
  - `phone_power.cpp`: battery polling, power key, power menu (AXP2101 through the BSP).
  - `phone_lock.cpp`: lock screen and screen power policy.
  - `phone_volume.cpp`: volume keys and volume panel.
  - `phone_audio.cpp`: speaker volume, the confirmation beep, and lending the speaker to the Music, Radio and Video apps and the AI Assistant.
  - `phone_storage.cpp`: microSD mount.
  - `phone_locale.cpp`: language, time zone and small NVS preferences.
  - `fonts/`: UI fonts with Vietnamese glyphs (Be Vietnam Pro, 12 to 28 px, SIL Open Font License, see `fonts/OFL.txt`) and the 120 px digits of the lock screen clock (Be Vietnam Pro Medium).
  - `tools/gen_lv_font.py`: script that generated the UI fonts from the TTF (Python with Pillow).
- `components/phone_app_level/`: the Level app.
- `components/phone_app_camera/`: the Camera app.
- `components/phone_app_gallery/`: the Gallery app.
- `components/phone_app_weather/`: the Weather app.
- `components/phone_app_music/`: the Music app (decoders from `espressif/esp_audio_codec`, fetched by the component manager). `music/` holds the ID3 parser, track library and background metadata service, adapted from the author's esp_chatbot_v1 project.
- `components/phone_app_led_rgb/`: the LED RGB app.
- `components/phone_app_recorder/`: the Recorder app.
- `components/phone_app_radio/`: the Radio app. `radio_player.cpp` (GMF simple player, HLS reader, HTTPS with the certificate bundle, spectrum) is ported from MetalioClaw4; `radio_stations.cpp` reads the JSON list. Versions pinned in its `idf_component.yml` keep `esp_audio_codec` 2.6 and `esp_codec_dev` 1.5 of the other apps (`gmf_io` 1.0.1 and later want `esp_codec_dev` 2.0 beta).
- `components/phone_app_video/`: the Video app.
- `components/phone_spectrum/`: the spectrum of the Music and Radio apps (`espressif/gmf_fft`).
- `components/esp_driver_sdmmc/`: a local copy of the ESP-IDF v6.1 SDMMC driver that writes with SET_BLOCK_COUNT (CMD23) instead of the automatic stop command (see its `README_LOCAL.md`).
- `components/phone_app_files/`: the Files app.
- `components/phone_app_assistant/`: the AI Assistant app. `assistant_chat.cpp` is the voice session (Opus from `espressif/esp_audio_codec`), `assistant_emote.cpp` the animated face; `eaf/` holds the EAF animation decoder of `espressif2022/esp_emote_gfx` (Apache-2.0) and `emote/` the eye animations (Espressif Emote Pack, license not confirmed, see the README there).
- `components/esp_xiaozhi/`: Xiaozhi protocol component of esp-iot-solution (Apache-2.0), copied from the author's esp_chatbot_v1 project; the language reported to the server is a Kconfig option here.
- `components/phone_app_usb_camera/`: the USB camera app (UVC class driver from `espressif/usb_host_uvc`, fetched by the component manager).
- `components/phone_app_second_screen/`: the Second screen app (USB device stack from `espressif/esp_tinyusb`, fetched by the component manager).
- `components/brookesia_app_calculator/`, `components/brookesia_app_game_2048/`: Calculator and 2048 from ESP-Brookesia, ported from its speaker system to the phone system. 2048 ignores swipes that start on a screen edge or on the lock screen.
- `components/brookesia_core/`: ESP-Brookesia core (phone system, GUI, stylesheets).
- `components/brookesia_app_squareline_demo/`: Squareline demo app.
- `components/phone_elf_apps/`: ELF apps from the card (API header, host, symbol list, ElfApp, the Apps app, `.bpk` reader); `sdk/`: the SDK for those apps; `tools/app_test/`: the app that tests every call; [`docs/elf_apps/`](elf_apps/README.md): the documentation.
- `components/phone_common_net/`: boot-time Wi-Fi for the RainMaker workflow; a no-op here (`CONFIG_RAINMAKER_APP_SUPPORT=n`).
- Board support package: `esp32p4c5_bsp` from GitHub, tag `v0.2.1`, pulled in by `main/idf_component.yml` (git source, `bsp/esp32p4c5_bsp`). To change the BSP alongside the phone, swap the git source for `override_path` to a local clone (the comment in that file shows it).

## Configuration

- `Phone example → Mount the microSD card at boot` (`CONFIG_PHONE_SD_CARD_MOUNT`, default on).
- The camera sensors SC2336 and OV2710 and the ISP pipeline controller of `esp_video` (auto exposure, white balance, color correction) are enabled in `sdkconfig.defaults`.
- The USB host control transfer size is raised to 4096 bytes in `sdkconfig.defaults`: webcams have configuration descriptors of several kilobytes, and the default 256 bytes makes their enumeration fail.
- TinyUSB has one vendor interface and one HID interface enabled in `sdkconfig.defaults`, for the Second screen app.
- Wi-Fi goes through ESP-Hosted over SDIO; `sdkconfig.defaults` selects the ESP32-C5 as the co-processor.
- 4G: `CONFIG_LWIP_PPP_SUPPORT`, `CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF` and a 1024-byte AT answer buffer (`CONFIG_ESP_MODEM_C_API_STR_MAX`) in `sdkconfig.defaults`.

## Notes

- LVGL is pinned to `^9` (9.5 in the current lock) by `brookesia_core`. A few `lv_part_t` conversions were changed to `LV_PART_MAIN` to build against it.
- The phone takes the default display and touch from LVGL (the BSP registers them), so no extra wiring is needed.
- The stock Brookesia fonts have no Vietnamese glyphs, so `main.cpp` replaces the 12 to 28 px system fonts in the stylesheet with the fonts of the Settings app (Be Vietnam Pro).
- The lock screen and the power menu are drawn on the LVGL system layer, above the Brookesia status and navigation bars.
- The Camera app task runs on core 0: the CSI and DSI controllers share one interrupt, which can only be attached on the core that started the display.
- Scrolling lists stop 36 px above the bottom edge (Weather, Gallery): a touch guard keeps the swipe-up for the navigation bar from scrolling them.
- The slot has no card-detect pin: a card inserted after boot needs a restart.
- The AXP2101 has no battery current measurement, so the About page shows voltage and fuel gauge level only.
- BOOT (GPIO35) is also wired to the DTR line of the CH343P USB-UART bridge: a terminal on that port that asserts DTR reads as "volume down" held.
