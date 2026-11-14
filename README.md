# BSP: ESP-Brookesia Phone example

## Overview

Runs the ESP-Brookesia Phone UI on the KSDIY ESP32-P4C5 4.3" board (480 x 800 panel, ST7102 over MIPI DSI, ST7123 touch). The phone uses the 480 x 800 stylesheet and installs the Squareline demo app from the app registry.

Wi-Fi and RainMaker are disabled (`CONFIG_RAINMAKER_APP_SUPPORT=n`), so the example does not need the ESP32-C5 radio.

## Hardware Requirements

* KSDIY ESP32-P4C5 4.3" board (ESP32-P4 rev v3.2, 16 MB flash, 32 MB PSRAM).

## Software Requirements

* ESP-IDF v6.1.

## Layout

- `main/`: `main.cpp` starts the BSP display, the LVGL lock, the phone UI and the app registry.
- `components/brookesia_core/`: ESP-Brookesia core (phone system, GUI, stylesheets).
- `components/brookesia_app_squareline_demo/`: Squareline demo app.
- `components/phone_common_net/`: network hooks; no-op when RainMaker is disabled.
- `../../bsp/esp32p4c5_bsp/`: board support package, pulled in through `main/idf_component.yml`.

## Build and Flash

Run the build from this directory:

```
idf.py set-target esp32p4
idf.py -p {port} flash monitor
```

To exit the serial monitor, type `Ctrl-]`.

## Notes

- LVGL is pinned to `^9` (9.5 in the current lock) by `brookesia_core`. A few `lv_part_t` conversions were changed to `LV_PART_MAIN` to build against it.
- The phone takes the default display and touch from LVGL (the BSP registers them), so no extra wiring is needed.
