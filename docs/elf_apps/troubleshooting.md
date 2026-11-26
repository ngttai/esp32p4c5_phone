# ELF apps: troubleshooting

[Index](README.md) · [Overview](overview.md) · [API reference](api.md) · [Package format](package.md) · [Tiếng Việt](vi/troubleshooting.md)

The phone writes a log line for every step. Connect the USB debug port and read the console (tags `ELFAPPS`, `ELFHOST`, `ELFSYM`).
Opening the serial port resets the board: start the log recorder first, then do the steps.

| Symptom | Likely cause | What to do |
|---|---|---|
| The `.bpk` is not in "Available" | Not in `/packages/`, the card was not ejected before pulling it, or the ZIP has no `manifest.json` at its root | Check the path; eject the card on the PC; press **Refresh**. The log has `<file>: ...` (for example `file not in the archive`, `not a ZIP file`). |
| The `.bpk` shows a red note and a disabled **Old** button | `runtime.api` is missing or is not the API version of the phone | Rebuild the app against the current `phone_elf_api.h` and set `"api": 3`. |
| **Install** says `a folder with this id is already in userapps` | An older folder with the same id is in `/userapps/` | Remove the installed app (or **Delete** the broken one), then install again. |
| **Install** says `already installed` | The id is installed | Remove it first. |
| A package under "Installed" has a note and a **Delete** button | The phone could not load it: another API version, `manifest.json` unreadable, id used twice | Read the note; **Delete** it, or fix the package and install it again. |
| The app icon appears but opening it shows a red text `cannot load the ELF (symbol or format)` | The ELF needs a symbol that is not in the list, or it was built for another architecture or without `-fPIC` | Look for `Can't find common <name>` in the log. Use only what `phone_elf_symbols.c` lists, or add the function there. Build with `sdk/Makefile` flags. |
| Opening shows `cannot read the ELF file` | `runtime.entry` names a file that is not in the package | Fix `entry`; rebuild the package. |
| Opening shows `too many ELF apps are open` | 4 ELF apps are already loaded | Close one. |
| The screen of the app is empty and the log says `entry returned -1` | `main` returned an error before building the screen: `phone_init` failed (the API version differs) or `argc < 2` | Build against the same `phone_elf_api.h` as the firmware. |
| A call does nothing and the log says `permission not in the manifest` | The app uses a group it did not list in `permissions` | Add the group to `manifest.json`, repackage, reinstall. The call returned `PHONE_ELF_E_PERM` (-1). |
| `fopen` returns `NULL` and the log says `refused` | The path is outside `app_dir()` / `pkg_dir()`, or you are writing in `pkg_dir()`, or the app lacks `files` | Use `path_in_data()` / `app_dir()`; add `"files"`; call `make_dirs` before the first write. |
| `fopen` works but the file is missing after a reinstall | The app was removed with "Delete the data too" on | Turn the switch off before removing if the data must stay. |
| `pref_set` returns -22 | Empty key, or longer than 15 characters | Shorten the key. |
| Vietnamese letters show as boxes | A custom font is set, or text was drawn outside the host's container | Use the default font; the container uses a font with Vietnamese glyphs. |
| The screen freezes for a moment | A callback blocks (`delay_ms`, long loops, large file writes) | Keep callbacks short; do the work in small steps from a timer. |
| **Remove** says `close the app first` | The app is open (maybe in the background) | Close it from the recents screen, then remove it. |
| After a crash the phone reboots | An ELF wrote to memory it does not own, or ran an invalid instruction | Read the panic dump in the log. There is no isolation between apps and the firmware: test new apps on a development board. |
| **Install** fails with `the launcher refused it` or `out of memory` | The launcher could not take one more app, or memory is low | Close apps and remove the ones you do not use; try again. |

## Reading the panic dump

A crash prints `Guru Meditation Error` with `MEPC` (the faulting address). Addresses from `0x48f...` to `0x490...` (PSRAM) are inside an ELF image; the log line
`ELFHOST [<id>] ... base 0x48f..` at load time gives the base of that app. To find the function, build the app with `-g` (keep the unstripped ELF) and look the offset `MEPC - base` up with the loader tool
`managed_components/espressif__elf_loader/tool/elf_addr2line.py` (or `riscv32-esp-elf-addr2line`): the ELF is position independent and linked from address 0.

## Useful log lines

| Line | Meaning |
|---|---|
| `ELFAPPS: installed 'Name' (id) as app N, permissions 0x..` | The package was registered on the launcher. |
| `ELFHOST: [id] N bytes, relocate 0, T us, base 0x.., perms 0x..` | The ELF was loaded (relocate 0 = success). |
| `ELFHOST: [id] entry returned 0` | `main` finished; the app now lives in callbacks. |
| `ELFHOST: [id] pause` / `resume` / `closing` / `closed` | Life cycle. `closed` shows the free PSRAM and SRAM after the unload. |
| `ELFSYM: [id] fopen(path, mode) refused` | The file sandbox refused a path. |
| `ELFHOST: [id] <call>: permission not in the manifest` | A call without its permission. |
| `ELFAPPS: uninstalled id and its data` / `, data kept` | The app was removed. |
