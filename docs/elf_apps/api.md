# ELF apps: API reference (version 3)

[Index](README.md) · [Overview](overview.md) · [Package format](package.md) · [Troubleshooting](troubleshooting.md) · [Tiếng Việt](vi/api.md)

The contract between the firmware and an app is `components/phone_elf_apps/include/phone_elf_api.h`. This page explains it. The SDK header
`sdk/phone_elf_sdk.h` wraps the calls with shorter names; the table below gives both.

## Entry point

```c
int main(int argc, char *argv[]);
```

| Argument | Value |
|---|---|
| `argc` | 2 |
| `argv[0]` | `const phone_elf_api_t *`: the API table |
| `argv[1]` | `phone_elf_ctx_t *`: the context of this app instance, the first argument of (almost) every call |

Build the screen under `root()` and return. Return `0` (the value is only logged). With the SDK:

```c
#include "phone_elf_sdk.h"
int main(int argc, char *argv[])
{
    if (phone_init(argc, argv) != 0) return -1;     // keeps the table and the context, checks the version
    ui_text(ui_label(ui_root()), "Hello");
    return 0;
}
```

Each running app has its **own copy** of the ELF image, so its global and static variables are private. Keep the state in globals.

## Rules

- **Threading.** The entry, every event callback, every timer callback and every life cycle handler run on the LVGL task with the LVGL lock held. Keep them short and never block:
  `delay_ms` in a callback freezes the screen. There are no threads.
- **Return values.** `0` or a positive value on success; a negative errno on failure: `PHONE_ELF_E_PERM` (-1, permission not in the manifest), `PHONE_ELF_E_IO` (-5), `PHONE_ELF_E_NOMEM` (-12), `PHONE_ELF_E_ARG` (-22).
  Functions that return pointers return `NULL` on failure; functions that return `void` ignore bad arguments (a refused call is logged by the host).
- **Objects.** LVGL objects are opaque `void *`. Everything created under `root()` is deleted when the app closes. `root()` itself cannot be deleted.
- **Colors** are `0xRRGGBB`. **Text** passed to the API is copied.
- **Versioning.** Members are only ever added at the end of the table. `size` is the size of the table in the firmware; an app built against an older header keeps working on a newer firmware.
  Check `api->size >= sizeof(phone_elf_api_t)` for "the whole table is there", or `PHONE_ELF_HAS(api, member)` before calling a member added in a later version. Never compare `size` for equality.
- **Libc.** Only the functions listed in `components/phone_elf_apps/phone_elf_symbols.c` can be linked. An ELF that needs another symbol is refused when it loads.

## Core

| API | SDK | Description |
|---|---|---|
| `void log(ctx, const char *text)` | `app_log`, `app_logf(fmt, ...)` | Write a line to the console log: `ELFHOST [<id>] <text>`. Not shown to the user. |
| `void close(ctx)` | `app_close` | Ask the phone to close this app. Takes effect after the current callback returns; `on_close` runs, then the screen is deleted. |
| `const char *tr(ctx, vi, en)` | `app_tr` | Returns `vi` when the phone language is Vietnamese, else `en`. The returned pointer is one of the arguments. |
| `uint32_t permissions(ctx)` | `app_permissions` | The `PHONE_ELF_PERM_*` bits granted by the manifest. |
| `uint32_t free_psram(ctx)`, `free_sram(ctx)` | | Free memory in bytes. |

## UI

`root()` is a full-screen container: dark background (`0x101820`), white text, 12 px padding, 6 px row gap, font 16 px. It is **not** a flex container until you call `obj_set_flex_column`.

| API | SDK | Description |
|---|---|---|
| `void *root(ctx)` | `ui_root` | The container of the app. |
| `void *obj_create(ctx, parent)` | `ui_box` | A transparent box: no background, border or padding, no scrollbar. |
| `void *label_create(ctx, parent)` | `ui_label` | A label; long text wraps (set a width with `obj_set_size(obj, -100, -1)`). |
| `void *button_create(ctx, parent)` | | A button; put a label inside it. The SDK's `ui_button(parent, text, cb, user)` does both (size 45% x 64 px). |
| `void *slider_create(ctx, parent)` | `ui_slider(parent, min, max, value)` | A slider (default range 0..100). |
| `void *switch_create(ctx, parent)` | `ui_switch` | An on/off switch. |
| `void obj_delete(ctx, obj)` | `ui_delete` | Delete an object and its children. |
| `void label_set_text(ctx, label, text)` | `ui_text` | Replace the text. |
| `void obj_set_size(ctx, obj, w, h)` | `ui_size` | Pixels; `-1` = fit the content; another negative value = percent of the parent (`-100` = 100%). |
| `void obj_set_pos(ctx, obj, x, y)` | `ui_pos` | Position inside the parent (ignored inside a flex container). |
| `void obj_align(ctx, obj, align, x, y)` | `ui_align` | `PHONE_ELF_ALIGN_CENTER`, `_TOP_MID`, `_BOTTOM_MID`, with an offset. |
| `void obj_set_bg_color(ctx, obj, rgb)` | `ui_bg` | Background color (also makes it opaque). |
| `void obj_set_text_color(ctx, obj, rgb)` | `ui_color` | Text color (inherited by children). |
| `void obj_set_font_size(ctx, obj, px)` | `ui_font_size` | Text size, 12..28 px (clamped), Vietnamese glyphs included. Default 16; inherited by children. |
| `void obj_set_radius(ctx, obj, r)` | `ui_radius` | Corner radius in pixels. |
| `void obj_set_hidden(ctx, obj, hidden)` | `ui_hide` | Hide or show an object. |
| `void obj_set_flex_column(ctx, obj)`, `obj_set_flex_row(ctx, obj)` | `ui_column`, `ui_row` | Lay the children out in a column, or in a row (spaced around, centered). |
| `void slider_set_range(ctx, s, min, max)` | | |
| `void slider_set_value(ctx, s, value)` | | |
| `int slider_get_value(ctx, s)` | | |
| `void switch_set_state(ctx, sw, on)` | | |
| `bool switch_get_state(ctx, sw)` | | |

### Events

```c
void obj_add_event_cb(ctx, obj, phone_elf_event_cb_t cb, int event, void *user_data);   // SDK: ui_on(obj, event, cb, user)
```

`cb` is `void (*)(void *event)`. Events: `PHONE_ELF_EV_CLICKED`, `PHONE_ELF_EV_VALUE_CHANGED` (sliders, switches), `PHONE_ELF_EV_LONG_PRESSED`.
Inside the callback use `event_target(event)` (the object that fired, SDK `ui_event_target`) and `event_user_data(event)` (the `user_data` you passed, `ui_event_user`).
At most 64 handlers per app; the 65th is refused and logged.

```c
static void on_tap(void *event) { count++; ui_text(label, "tapped"); }
...
ui_button(root, "Tap me", on_tap, 0);
```

## Timers

| API | SDK | Description |
|---|---|---|
| `void *timer_create(ctx, period_ms, cb, user_data)` | `timer_start` | Call `cb(user_data)` every `period_ms` on the LVGL task. Returns a handle, or `NULL` (period 0, or 8 timers already). |
| `void timer_delete(ctx, timer)` | `timer_stop` | Stop a timer. |

Timers are **paused while the app is in the background** (unless the app registered `PHONE_ELF_LC_KEEP_TIMERS`) and resume when it comes back.
The host deletes the timers that are left when the app closes.

## Time

| API | SDK | Description |
|---|---|---|
| `uint32_t millis(ctx)` | `app_millis` | Milliseconds since boot. |
| `int64_t epoch(ctx)` | `app_epoch` | Seconds since 1970, or `0` while the clock is not set. |
| `void delay_ms(ctx, ms)` | | Sleep. **Blocks the LVGL task** (the whole screen): use it at start-up only. |

## Files

| API | SDK | Permission | Description |
|---|---|---|---|
| `const char *app_dir(ctx)` | `app_data_dir` | | `/sdcard/appdata/<id>`: the data folder of the app. |
| `int make_dirs(ctx, file_path)` | | `files` | Create the folders above a file; the path must be inside `app_dir()`. Returns `0`, `PHONE_ELF_E_ARG` or `PHONE_ELF_E_PERM`. |
| `const char *pkg_dir(ctx)` | `app_pkg_dir` | | `/sdcard/userapps/<id>`: the unpacked package, read only (resources, data files). |

Use the normal libc calls on those paths (`fopen`, `fread`, `fwrite`, `fseek`, `fclose`, `remove`, `rename`, ...). `fopen`, `remove` and `rename` are sandboxed by the host:

| Path | `fopen` read | `fopen` write | `remove`, `rename` |
|---|---|---|---|
| inside `app_dir()` | needs `files` | needs `files` | needs `files` |
| inside `pkg_dir()` | allowed | refused (`NULL`) | refused |
| anywhere else | refused | refused | refused |

A refused call returns `NULL` / `-1` and logs `ELFSYM [<id>] fopen(...) refused`. The SDK's `path_in_data(buf, size, "name")` builds a path inside `app_dir()`.
Create the folder first: `make_dirs` on a file path creates the folders above it.

## Settings

| API | SDK | Description |
|---|---|---|
| `int32_t pref_get(ctx, key, def)` | `pref_int` | Read a 32-bit value; `def` when the key does not exist. |
| `int pref_set(ctx, key, value)` | `pref_save` | Write a 32-bit value and commit it to flash. `0`, or `PHONE_ELF_E_ARG` (empty key or longer than 15 characters), `PHONE_ELF_E_IO`. |

Each app has its own NVS namespace, apart from the system and from other apps. It is erased when the app is removed with its data. No permission needed.
A write goes to flash: do not call it in a tight loop; save on a button press or in `on_close`.

## Hardware

Each call needs the permission shown, otherwise it returns `PHONE_ELF_E_PERM` and does nothing.

| API | SDK | Permission | Description |
|---|---|---|---|
| `int beep(ctx)` | `hw_beep` | `audio` | Play a short tone at the current volume; returns at once. |
| `int volume_get(ctx)` | | `audio` | Speaker volume in percent. |
| `int led_set(ctx, rgb, effect, period_ms)` | `hw_led` | `led` | Show an effect on the board LED. Effects: `PHONE_ELF_LED_SOLID`, `_BLINK`, `_BREATHE`, `_RAINBOW` (ignores the color); `period_ms` <= 0 means 1000. Low priority: the Recorder's red blink wins. |
| `int led_off(ctx)` | `hw_led_off` | `led` | Give the LED back. The host also does it when the app closes. |
| `int imu_read(ctx, phone_elf_imu_t *out)` | `hw_imu` | `imu` | Raw accelerometer and gyroscope sample (LSM6DS3TR-C): accel +-2 g, 0.061 mg per count; gyro 250 dps, 8.75 mdps per count. The sensor is mounted rotated 90 degrees: X points to the bottom edge of the screen, Y to the left. |
| `int battery_read(ctx, phone_elf_battery_t *out)` | `hw_battery` | `battery` | `percent` (-1 without battery), `voltage_mv`, `charging_usb` (USB supply present). |
| `int net_online(ctx)` | `hw_online` | `net` | `1` when the phone has Internet (Wi-Fi or 4G), `0` when not. There are no network calls yet: apps cannot open sockets. |

## Life cycle

```c
typedef struct {
    void (*on_pause)(void *user);    // the app goes to the background
    void (*on_resume)(void *user);   // the app comes back
    void (*on_close)(void *user);    // the app is closed for good: save state here
    void *user;
    uint32_t flags;                  // PHONE_ELF_LC_KEEP_TIMERS
} phone_elf_lifecycle_t;

int  lifecycle_set(ctx, const phone_elf_lifecycle_t *handlers);   // SDK: app_lifecycle(on_pause, on_resume, on_close, user, flags)
bool is_foreground(ctx);                                          // SDK: app_foreground
```

- `lifecycle_set` copies the structure; `NULL` handlers are fine. Call it from `main`.
- **`on_pause`**: Home, or another app opened. The screen and the memory stay. Timers are paused unless `flags` has `PHONE_ELF_LC_KEEP_TIMERS`.
- **`on_resume`**: the app is back in the foreground; paused timers continue.
- **`on_close`**: the Close button (`close(ctx)`), Back, or the launcher closing the oldest app. The API still works here (`pref_set`, files): save state. It runs under the LVGL lock, so keep it short. Right after it, the timers and the screen are deleted.
- **Not sent:** turning the screen off or locking it does not call `on_pause`; the app stays in the foreground.
- An app that is closed and opened again starts from `main` again with fresh globals.

## Constants

```c
#define PHONE_ELF_API_VERSION 3
PHONE_ELF_PERM_LED, _AUDIO, _IMU, _BATTERY, _NET, _FILES            // 1 << 0 ... 1 << 5
PHONE_ELF_EV_CLICKED = 0, _VALUE_CHANGED = 1, _LONG_PRESSED = 2
PHONE_ELF_ALIGN_CENTER = 0, _TOP_MID = 1, _BOTTOM_MID = 2
PHONE_ELF_LED_SOLID = 0, _BLINK = 1, _BREATHE = 2, _RAINBOW = 3
PHONE_ELF_LC_KEEP_TIMERS = 1
```

## Version history

| Version | Changes |
|---|---|
| 3 | Context argument in every call; several apps at once; `size`; error codes; permissions; timers, switch, delete, position, radius, hidden; 32-bit settings in a per-app namespace; `pkg_dir`; life cycle; `obj_set_font_size`. First published version. |

Versions 1 and 2 were internal prototypes and are not supported: packages must say `"api": 3`.
