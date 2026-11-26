# SDK of the ELF apps (API v3)

An app is an ELF loaded into PSRAM by the phone, packed with its `manifest.json` into a `.bpk`. Copy the `.bpk` to `/packages/` on
the memory card, open **Ứng dụng** (Apps) on the phone and press **Cài** (Install): the app appears on the launcher.

## Build

```
cd sdk
make APP=examples/hello        # -> out/hello.bpk
```

Needs the RISC-V toolchain of ESP-IDF (`~/.espressif/tools/riscv32-esp-elf/...`, or `make TC=<path to riscv32-esp-elf>`), Python 3.
No ESP-IDF project is needed.

## An app

- `main(int argc, char *argv[])`: call `phone_init(argc, argv)` first, build the screen under `ui_root()`, return. Everything later happens in
  callbacks (buttons, timers, life cycle) on the LVGL task: keep them short, never block.
- `phone_elf_sdk.h`: short wrappers (`ui_label`, `ui_button`, `timer_start`, `pref_int`...). `phone_elf_api.h` (in `components/phone_elf_apps/include`) is the
  real API with the rules of every call, and `PHONE(fn, ...)` reaches any member.
- Only these libc functions can be linked: memory, strings, ctype, math, `printf` family, files (`fopen` ... sandboxed). The list is
  `components/phone_elf_apps/phone_elf_symbols.c`; an ELF that needs another symbol is refused when it loads. No network, no threads.
- Files: `app_data_dir()` (`/sdcard/appdata/<id>`, needs the `files` permission) and `app_pkg_dir()` (the package, read only).
- Settings: `pref_int(key, default)` / `pref_save(key, value)`: 32-bit values, key up to 15 characters, private to the app.
- Life cycle: `app_lifecycle(on_pause, on_resume, on_close, user, flags)`. Timers pause in the background (flag `PHONE_ELF_LC_KEEP_TIMERS` keeps them).
  `on_close` runs before the screen is deleted: save state there.

## manifest.json

```json
{
  "package": { "id": "hello_sdk", "name": { "en": "Hello", "vi": "Xin chào" }, "version": "0.1.0", "icon": "icon.png" },
  "runtime": { "type": "ELF", "entry": "hello.elf", "api": 3 },
  "permissions": ["led", "audio", "imu", "battery", "net", "files"]
}
```

- `id`: letters, digits, `_ - .`, up to 32. Two packages with the same id are the same app.
- `runtime.api` is required and must equal the API version of the phone: a package built for another version is refused with a message.
- `permissions`: the groups of calls the app uses (the rest return `PHONE_ELF_E_PERM`). The permissions are a declaration, not a sandbox:
  install only packages you trust.
- `icon`: PNG of any size (shown at 126 x 126); the phone draws one when there is none.
