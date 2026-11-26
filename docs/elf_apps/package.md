# ELF apps: package format (`.bpk`)

[Index](README.md) · [Overview](overview.md) · [API reference](api.md) · [Troubleshooting](troubleshooting.md) · [Tiếng Việt](vi/package.md)

An app is shipped as a **`.bpk`**: a plain ZIP file with `manifest.json` at its root, the ELF it names, and an optional icon.

```
hello.bpk
  manifest.json
  hello.elf
  icon.png          (optional)
  res/...           (optional: any files the app reads with pkg_dir())
```

## On the card

| Path | Content |
|---|---|
| `/packages/<name>.bpk` | packages waiting to be installed (the "Available" list of the Apps app) |
| `/userapps/<id>/` | installed packages, unpacked (the "Installed" list; loaded at boot) |
| `/appdata/<id>/` | data of the app (`app_dir()`); kept or deleted when the app is removed |

`/apps/` is not used: it is left to apps of ESP-Brookesia 0.8.

## `manifest.json`

```json
{
  "package": {
    "id": "hello_sdk",
    "name": { "en": "Hello", "vi": "Xin chào" },
    "version": "0.1.0",
    "icon": "icon.png"
  },
  "runtime": {
    "type": "ELF",
    "entry": "hello.elf",
    "api": 3
  },
  "permissions": ["led", "audio", "imu", "battery", "net", "files"]
}
```

| Field | Required | Meaning |
|---|---|---|
| `package.id` | yes | Unique id: letters, digits, `_`, `-`, `.`, at most 32 characters. Also the name of the folder in `/userapps/` and `/appdata/`, the settings namespace and the LED owner. Two packages with the same id are the same app. |
| `package.name` | no | A string, or `{ "en": "...", "vi": "..." }`: the name under the icon, chosen by the language of the phone. Defaults to the id. |
| `package.version` | no | Shown in the list (free text). Not compared yet. |
| `package.icon` | no | Path of a PNG inside the package, default `icon.png`. Any size, shown at 126 x 126 (nearest neighbour; draw it at 126 x 126). Without an icon, the phone draws one from the id. |
| `runtime.type` | yes | Must be `ELF`. Other types are skipped. |
| `runtime.entry` | yes | Path of the ELF inside the package (relative, no `..`, not absolute). |
| `runtime.api` | yes | API version the ELF was built for. Must equal the version of the phone (now **3**), otherwise the package is refused. |
| `permissions` | no | Array of groups the app uses. Default: none. |

Unknown fields are ignored. A permission name the phone does not know is ignored with a warning in the log.

### Permissions

| Name | Calls it unlocks |
|---|---|
| `led` | `led_set`, `led_off` |
| `audio` | `beep`, `volume_get` |
| `imu` | `imu_read` |
| `battery` | `battery_read` |
| `net` | `net_online` |
| `files` | `make_dirs`, and `fopen` / `remove` / `rename` inside `app_dir()` |

Reading the package folder (`pkg_dir()`), the settings, the UI, timers and the life cycle need no permission. See [overview.md](overview.md#security-model) for what permissions do and do not protect.

## ZIP limits

Stored or deflate entries only. No ZIP64, no encryption, no multi-disk archives. At most 256 entries, 4 MB per file, 16 MB unpacked, central directory up to 128 KB.
Entries with absolute paths, `..` or `\` make the whole package fail. `META-INF/` is skipped (reserved for signatures, which are not checked yet).
Each file is checked against its CRC-32 while it is unpacked.

## Building a package

With the SDK (no ESP-IDF project needed):

```
cd sdk
make APP=examples/hello          # build/hello.elf, out/hello.bpk
```

To pack a folder yourself (ELF already built): `python3 tools/make_bpk.py <folder> <out.bpk>`. The folder must contain `manifest.json`.
The ELF must be built the way the loader expects: position independent, no startup files, entry `app_main` (`main` renamed), see [`sdk/Makefile`](../../sdk/Makefile).

## Installing, updating, removing

- **Install:** copy the `.bpk` to `/packages/` on the card (eject the card properly), open **Apps**, press **Refresh**, then **Install**.
  The package is unpacked to `/userapps/<id>/`; a failed install leaves nothing behind.
- **Update:** remove the app, then install the new `.bpk` (same id). Keep the data by turning off "Delete the data too" before removing.
  There is no in-place update or version comparison yet.
- **Remove:** press **Remove** twice (the first press asks "Sure?" for 3 seconds). An app that is running cannot be removed: close it first.
  With "Delete the data too" on (default), its settings and `/appdata/<id>/` are deleted too.
- **Packages that do not load:** a folder in `/userapps/` that cannot be loaded (for example built for another API version) is listed under "Installed" with the reason and a **Delete** button.
  A `.bpk` of another API version is listed under "Available" with a disabled **Old** button.
