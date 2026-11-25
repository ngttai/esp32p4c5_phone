/*
 * Apps loaded from the memory card: /sdcard/userapps/<folder>/manifest.json + the ELF it names. Every package found
 * becomes an app on the launcher, installed while the phone runs (nothing is built into the firmware).
 *
 * manifest.json (the fields of an ESP-Brookesia 0.8 package, so the same package can move there later):
 *   { "package": { "id": "app_test", "name": { "en": "App test", "vi": "Thử app" }, "version": "0.1.0" },
 *     "runtime": { "type": "ELF", "entry": "app_test.elf" } }
 */
#pragma once

namespace esp_brookesia::systems::phone {
class Phone;
}

/** Start the scan in the background: waits for the card, installs the apps found. Call after Phone::begin(). */
void phone_elf_apps_start(esp_brookesia::systems::phone::Phone *phone);
