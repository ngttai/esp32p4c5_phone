# ELF apps: overview

[Index](README.md) · [API reference](api.md) · [Package format](package.md) · [Troubleshooting](troubleshooting.md) · [Tiếng Việt](vi/overview.md)

ELF apps are small native apps that live on the memory card. Copy a `.bpk` package to the card, install it from the **Apps**
app of the phone, and it appears on the launcher. Nothing is built into the firmware, nothing is flashed.

## What you get

- Native speed: the app is RISC-V machine code loaded into PSRAM (no interpreter).
- A small, versioned C API for UI (LVGL widgets), timers, settings, files, and a few hardware services (LED, speaker, IMU, battery, network state).
- Several apps at the same time, each with its own memory image, settings and permissions.
- Install, update by reinstall, and remove at run time, without a reboot.

## How it fits together

```
 memory card                                   firmware (components/phone_elf_apps)
 -----------                                   --------------------------------------------------
 /packages/*.bpk        --install-->  Apps app (ElfAppManager)   unpack, check manifest.json, list, remove
 /userapps/<id>/                      |  scan at boot + on install
   manifest.json, app.elf, icon.png   v
 /appdata/<id>/   (data of the app)   ElfApp (one per package)   a launcher icon, life cycle: run / pause / resume / close
                                      |
                                      v
                                      host (phone_elf_host.c)    one instance per running app: ELF image, context,
                                      |                          permissions, timers, event handlers, NVS namespace
                                      v
                                      loader (espressif/elf_loader)  relocates the ELF into PSRAM; symbols = phone_elf_symbols.c
```

| Part | File | Job |
|---|---|---|
| API table | `include/phone_elf_api.h` | The contract between the firmware and an app (version 3). |
| Host | `phone_elf_host.c` | Loads one ELF, builds its screen, implements every API call, enforces permissions, runs the handlers. Up to 4 instances. |
| Symbol list | `phone_elf_symbols.c` | The only libc functions an app can link against; sandboxed `fopen`, `remove`, `rename`. |
| ElfApp, Apps app | `phone_elf_apps.cpp` | One `ElfApp` per installed package (launcher entry, life cycle). The Apps app lists, installs and removes packages. |
| ZIP reader | `phone_zip.c` | Reads `.bpk` files (ZIP, stored or deflate). |
| SDK | `sdk/` | `phone_elf_sdk.h`, a standalone Makefile, an example app. |

## Life of an app

1. **Install.** The Apps app reads `manifest.json` out of `/packages/<name>.bpk`, checks it (`runtime.api`, id), unpacks the package into
   `/userapps/<id>/`, and registers a launcher entry. No reboot.
2. **Open.** Brookesia calls `ElfApp::run()`: the host reads the ELF, relocates it into PSRAM (a few ms), creates the full-screen container
   (`root()`), and calls `main(argc, argv)` with the API table and the context of this instance. `main` builds the screen and returns.
3. **Run.** From now on the app lives in callbacks: events, timers, life cycle handlers. All run on the LVGL task.
4. **Background.** Going Home or opening another app calls `on_pause`; the screen stays alive, the timers pause (unless the app asked to keep them).
   Coming back calls `on_resume`.
5. **Close.** The Close button, Back, or the launcher closing the oldest app calls `on_close` (the screen still exists: save state), then the host
   deletes the timers and the screen and unloads the ELF.
6. **Remove.** The Apps app deletes `/userapps/<id>/` and, if the switch "Delete the data too" is on, the settings (NVS namespace) and `/appdata/<id>/`.

At boot the phone scans `/userapps/` and registers every package it can load. A package that cannot be loaded (wrong API version, bad manifest)
is listed in the Apps app with the reason and a **Delete** button.

## Memory and limits

| Item | Value |
|---|---|
| ELF apps loaded at once | 4 (`PHONE_ELF_HOST_MAX_APPS`); Brookesia also closes the oldest of 3 running apps |
| Memory of an app | its ELF image in PSRAM (a few KB), its `malloc`s in PSRAM, about 63 KB of PSRAM for its icon (shared by id) |
| Timers per app | 8 |
| Event handlers per app | 64 (one per `obj_add_event_cb` call) |
| Settings key | up to 15 characters, 32-bit values |
| Package | 256 files, 4 MB per file, 16 MB unpacked, `manifest.json` up to 16 KB, icon up to 256 KB |

There is no per-app memory quota yet: an app can `malloc` until PSRAM is gone.

## Security model

The mechanisms below stop **mistakes and casual misuse**. They do not stop a hostile ELF.

- **Permissions** (`"permissions"` in `manifest.json`): calls of a group the manifest did not ask for fail with `PHONE_ELF_E_PERM`.
- **Symbol list**: the loader exports nothing of its own (`CONFIG_ELF_LOADER_LIBC_SYMBOLS` and `CONFIG_ELF_LOADER_ESPIDF_SYMBOLS` are off). An app can only link
  against `phone_elf_symbols.c`: strings, memory, math, `printf`, and files. No sockets, no NVS, no reset, no threads. An ELF that needs anything else is refused at load.
- **File sandbox**: `fopen`, `remove`, `rename` reach only `/sdcard/appdata/<id>/` (with the `files` permission) and the package folder (read only).
- **Own settings**: each app has its own NVS namespace.

What is **not** protected: there is no MMU, and the firmware runs with `CONFIG_ESP_SYSTEM_MEMPROT` off (the PMP memory protection maps the PSRAM heap non-executable,
so an ELF cannot run there). An ELF can read and write any memory and jump to any address, including the memory of other apps and of the firmware.
There is no signature check on packages yet. **Install only packages you trust.**

## Required configuration

These are set in `sdkconfig.defaults` of the phone project:

| Option | Value | Why |
|---|---|---|
| `CONFIG_ESP_SYSTEM_MEMPROT` | off | code loaded into PSRAM must be executable |
| `CONFIG_ELF_LOADER_LIBC_SYMBOLS`, `CONFIG_ELF_LOADER_ESPIDF_SYMBOLS` | off | the app can only use the chosen symbol list |
| `CONFIG_LV_USE_LODEPNG` | on | PNG icons inside packages |
| `CONFIG_ELF_LOADER_LOAD_PSRAM` | on (default with SPIRAM) | load into PSRAM |

## Relation to ESP-Brookesia 0.8

The package layout (`manifest.json` with `package` / `runtime` fields) follows the ESP-Brookesia 0.8 app package so that tools and packages can be shared later.
The phone does **not** use the Brookesia 0.8 framework: it runs the older `brookesia_core`, and the ELF runtime of Brookesia 0.8 only loads an ELF and calls it
(no host API). The host, the API, the Apps app and the `.bpk` reader here are our own work. `runtime.api` and `permissions` are our additions.

## Status

Works on the board: install, open, several apps at once, life cycle, permissions, sandboxed files, per-app settings, removal with data wipe, `.bpk` with icon, standalone SDK.
Not done yet (see [package.md](package.md) and the design log `PLAN_EXT_APPS.md`): package signatures, version updates for the same id, automatic card rescan,
`pause` when the screen turns off, richer UI widgets, audio, HTTP, per-app memory quota.
