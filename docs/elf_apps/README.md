# ELF apps

Native apps on the memory card of the KSDIY ESP32-P4C5 phone: copy a `.bpk` to the card, install it from the **Apps** app, and it shows up on the launcher.
No rebuild, no flash. Language: English (default) · [Tiếng Việt](vi/README.md).

| Document | Read it for |
|---|---|
| [overview.md](overview.md) | what it is, how the parts fit together, life of an app, limits, security model, required configuration |
| [api.md](api.md) | reference of the C API (version 3): every call, permissions, life cycle, rules |
| [package.md](package.md) | the `.bpk` format, `manifest.json` fields, permissions, ZIP limits, install / update / remove |
| [troubleshooting.md](troubleshooting.md) | symptoms, causes and fixes; reading the log and a panic dump |
| [../../sdk/README.md](../../sdk/README.md) | the SDK: build an app and a `.bpk` without an ESP-IDF project |

## Quick start

```
cd sdk
make APP=examples/hello                       # -> out/hello.bpk
cp out/hello.bpk /media/$USER/<card>/packages/   # then eject the card
```

On the phone: **Apps** -> **Refresh** -> **Install** next to "Hello". The new icon appears on the launcher.
The source is `sdk/examples/hello/hello.c` (about 70 lines); the test app that uses every call is `tools/app_test/main/app_test.c`.

## Where the code is

| Path | Content |
|---|---|
| `components/phone_elf_apps/` | firmware side: API header, host, symbol list, ElfApp and Apps app, ZIP reader |
| `sdk/` | app side: `phone_elf_sdk.h`, `Makefile`, example |
| `tools/app_test/` | test app, builds the sample packages in `sdcard/packages/` |
| `tools/make_bpk.py`, `tools/make_icons.py` | pack a folder into a `.bpk`; draw the sample icons |
