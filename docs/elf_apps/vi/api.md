# Ứng dụng ELF: tham chiếu API (phiên bản 3)

[Mục lục](README.md) · [Tổng quan](overview.md) · [Định dạng gói](package.md) · [Xử lý sự cố](troubleshooting.md) · [English](../api.md)

Hợp đồng giữa firmware và app là `components/phone_elf_apps/include/phone_elf_api.h`. Trang này giải thích nó. Header SDK
`sdk/phone_elf_sdk.h` bọc các lời gọi bằng tên ngắn hơn; bảng dưới ghi cả hai.

## Điểm vào

```c
int main(int argc, char *argv[]);
```

| Tham số | Giá trị |
|---|---|
| `argc` | 2 |
| `argv[0]` | `const phone_elf_api_t *`: bảng API |
| `argv[1]` | `phone_elf_ctx_t *`: ngữ cảnh của instance app này, tham số đầu tiên của (hầu hết) mọi lời gọi |

Dựng màn hình dưới `root()` rồi trả về. Trả `0` (giá trị chỉ được ghi log). Với SDK:

```c
#include "phone_elf_sdk.h"
int main(int argc, char *argv[])
{
    if (phone_init(argc, argv) != 0) return -1;     // giữ bảng và ngữ cảnh, kiểm phiên bản
    ui_text(ui_label(ui_root()), "Xin chào");
    return 0;
}
```

Mỗi app đang chạy có **bản sao ảnh ELF riêng**, nên biến toàn cục và biến `static` là riêng. Hãy giữ trạng thái trong biến toàn cục.

## Quy tắc

- **Luồng.** Điểm vào, mọi callback sự kiện, callback timer và handler vòng đời đều chạy trên tác vụ LVGL, giữ khóa LVGL. Hãy giữ chúng ngắn và không bao giờ chặn:
  `delay_ms` trong callback làm đứng màn hình. Không có thread.
- **Giá trị trả về.** `0` hoặc số dương khi thành công; errno âm khi lỗi: `PHONE_ELF_E_PERM` (-1, quyền không có trong manifest), `PHONE_ELF_E_IO` (-5), `PHONE_ELF_E_NOMEM` (-12), `PHONE_ELF_E_ARG` (-22).
  Hàm trả con trỏ trả `NULL` khi lỗi; hàm trả `void` bỏ qua tham số sai (lời gọi bị từ chối được host ghi log).
- **Đối tượng.** Đối tượng LVGL là `void *` mờ. Mọi thứ tạo dưới `root()` bị xóa khi app đóng. Bản thân `root()` không xóa được.
- **Màu** là `0xRRGGBB`. **Chuỗi** đưa vào API được chép lại.
- **Phiên bản.** Chỉ thêm thành viên vào cuối bảng. `size` là kích thước bảng trong firmware; app dựng với header cũ vẫn chạy trên firmware mới.
  Kiểm `api->size >= sizeof(phone_elf_api_t)` cho "đủ cả bảng", hoặc `PHONE_ELF_HAS(api, member)` trước khi gọi thành viên thêm ở phiên bản sau. Không bao giờ so `size` bằng nhau.
- **Libc.** Chỉ các hàm trong `components/phone_elf_apps/phone_elf_symbols.c` liên kết được. ELF cần symbol khác bị từ chối lúc nạp.

## Lõi

| API | SDK | Mô tả |
|---|---|---|
| `void log(ctx, const char *text)` | `app_log`, `app_logf(fmt, ...)` | Ghi một dòng vào log console: `ELFHOST [<id>] <text>`. Người dùng không thấy. |
| `void close(ctx)` | `app_close` | Xin điện thoại đóng app này. Có hiệu lực sau khi callback hiện tại trả về; `on_close` chạy, rồi màn hình bị xóa. |
| `const char *tr(ctx, vi, en)` | `app_tr` | Trả `vi` khi ngôn ngữ điện thoại là tiếng Việt, ngược lại `en`. Con trỏ trả về là một trong hai tham số. |
| `uint32_t permissions(ctx)` | `app_permissions` | Các bit `PHONE_ELF_PERM_*` mà manifest cấp. |
| `uint32_t free_psram(ctx)`, `free_sram(ctx)` | | Bộ nhớ trống, tính bằng byte. |

## Giao diện

`root()` là vùng chứa toàn màn hình: nền tối (`0x101820`), chữ trắng, đệm 12 px, khoảng cách hàng 6 px, font 16 px. Nó **không** là flex cho tới khi bạn gọi `obj_set_flex_column`.

| API | SDK | Mô tả |
|---|---|---|
| `void *root(ctx)` | `ui_root` | Vùng chứa của app. |
| `void *obj_create(ctx, parent)` | `ui_box` | Một hộp trong suốt: không nền, viền, đệm, không thanh cuộn. |
| `void *label_create(ctx, parent)` | `ui_label` | Nhãn; chữ dài tự xuống dòng (đặt chiều rộng bằng `obj_set_size(obj, -100, -1)`). |
| `void *button_create(ctx, parent)` | | Nút; đặt một nhãn vào trong. `ui_button(parent, text, cb, user)` của SDK làm cả hai (cỡ 45% x 64 px). |
| `void *slider_create(ctx, parent)` | `ui_slider(parent, min, max, value)` | Thanh trượt (mặc định 0..100). |
| `void *switch_create(ctx, parent)` | `ui_switch` | Công tắc bật/tắt. |
| `void obj_delete(ctx, obj)` | `ui_delete` | Xóa đối tượng và các con của nó. |
| `void label_set_text(ctx, label, text)` | `ui_text` | Thay chữ. |
| `void obj_set_size(ctx, obj, w, h)` | `ui_size` | Điểm ảnh; `-1` = vừa nội dung; số âm khác = phần trăm của cha (`-100` = 100%). |
| `void obj_set_pos(ctx, obj, x, y)` | `ui_pos` | Vị trí trong cha (bỏ qua trong vùng flex). |
| `void obj_align(ctx, obj, align, x, y)` | `ui_align` | `PHONE_ELF_ALIGN_CENTER`, `_TOP_MID`, `_BOTTOM_MID`, kèm độ lệch. |
| `void obj_set_bg_color(ctx, obj, rgb)` | `ui_bg` | Màu nền (đồng thời làm nó đục). |
| `void obj_set_text_color(ctx, obj, rgb)` | `ui_color` | Màu chữ (con kế thừa). |
| `void obj_set_font_size(ctx, obj, px)` | `ui_font_size` | Cỡ chữ, 12..28 px (kẹp), có đủ dấu tiếng Việt. Mặc định 16; con kế thừa. |
| `void obj_set_radius(ctx, obj, r)` | `ui_radius` | Bo góc, điểm ảnh. |
| `void obj_set_hidden(ctx, obj, hidden)` | `ui_hide` | Ẩn hoặc hiện đối tượng. |
| `void obj_set_flex_column(ctx, obj)`, `obj_set_flex_row(ctx, obj)` | `ui_column`, `ui_row` | Xếp các con theo cột, hoặc theo hàng (cách đều, căn giữa). |
| `void slider_set_range(ctx, s, min, max)` | | |
| `void slider_set_value(ctx, s, value)` | | |
| `int slider_get_value(ctx, s)` | | |
| `void switch_set_state(ctx, sw, on)` | | |
| `bool switch_get_state(ctx, sw)` | | |

### Sự kiện

```c
void obj_add_event_cb(ctx, obj, phone_elf_event_cb_t cb, int event, void *user_data);   // SDK: ui_on(obj, event, cb, user)
```

`cb` là `void (*)(void *event)`. Sự kiện: `PHONE_ELF_EV_CLICKED`, `PHONE_ELF_EV_VALUE_CHANGED` (thanh trượt, công tắc), `PHONE_ELF_EV_LONG_PRESSED`.
Trong callback dùng `event_target(event)` (đối tượng phát sinh, SDK `ui_event_target`) và `event_user_data(event)` (`user_data` bạn truyền vào, `ui_event_user`).
Tối đa 64 handler mỗi app; cái thứ 65 bị từ chối và ghi log.

```c
static void on_tap(void *event) { count++; ui_text(label, "đã bấm"); }
...
ui_button(root, "Bấm thử", on_tap, 0);
```

## Timer

| API | SDK | Mô tả |
|---|---|---|
| `void *timer_create(ctx, period_ms, cb, user_data)` | `timer_start` | Gọi `cb(user_data)` mỗi `period_ms` trên tác vụ LVGL. Trả một handle, hoặc `NULL` (chu kỳ 0, hoặc đã đủ 8 timer). |
| `void timer_delete(ctx, timer)` | `timer_stop` | Dừng một timer. |

Timer **tạm dừng khi app ở nền** (trừ khi app đăng ký `PHONE_ELF_LC_KEEP_TIMERS`) và chạy tiếp khi app quay lại.
Host xóa các timer còn lại khi app đóng.

## Thời gian

| API | SDK | Mô tả |
|---|---|---|
| `uint32_t millis(ctx)` | `app_millis` | Mili giây từ lúc khởi động. |
| `int64_t epoch(ctx)` | `app_epoch` | Giây từ 1970, hoặc `0` khi chưa đặt đồng hồ. |
| `void delay_ms(ctx, ms)` | | Ngủ. **Chặn tác vụ LVGL** (cả màn hình): chỉ dùng lúc khởi tạo. |

## Tệp

| API | SDK | Quyền | Mô tả |
|---|---|---|---|
| `const char *app_dir(ctx)` | `app_data_dir` | | `/sdcard/appdata/<id>`: thư mục dữ liệu của app. |
| `int make_dirs(ctx, file_path)` | | `files` | Tạo các thư mục phía trên một tệp; đường dẫn phải nằm trong `app_dir()`. Trả `0`, `PHONE_ELF_E_ARG` hoặc `PHONE_ELF_E_PERM`. |
| `const char *pkg_dir(ctx)` | `app_pkg_dir` | | `/sdcard/userapps/<id>`: gói đã giải nén, chỉ đọc (tài nguyên, tệp dữ liệu). |

Dùng các lời gọi libc thông thường trên các đường dẫn đó (`fopen`, `fread`, `fwrite`, `fseek`, `fclose`, `remove`, `rename`, ...). `fopen`, `remove` và `rename` có sandbox do host áp dụng:

| Đường dẫn | `fopen` đọc | `fopen` ghi | `remove`, `rename` |
|---|---|---|---|
| trong `app_dir()` | cần `files` | cần `files` | cần `files` |
| trong `pkg_dir()` | được | từ chối (`NULL`) | từ chối |
| nơi khác | từ chối | từ chối | từ chối |

Lời gọi bị từ chối trả `NULL` / `-1` và ghi log `ELFSYM [<id>] fopen(...) refused`. `path_in_data(buf, size, "tên")` của SDK dựng đường dẫn trong `app_dir()`.
Tạo thư mục trước: `make_dirs` trên đường dẫn tệp tạo các thư mục phía trên nó.

## Cài đặt

| API | SDK | Mô tả |
|---|---|---|
| `int32_t pref_get(ctx, key, def)` | `pref_int` | Đọc giá trị 32 bit; trả `def` khi khóa chưa có. |
| `int pref_set(ctx, key, value)` | `pref_save` | Ghi giá trị 32 bit và commit xuống flash. `0`, hoặc `PHONE_ELF_E_ARG` (khóa rỗng hoặc dài quá 15 ký tự), `PHONE_ELF_E_IO`. |

Mỗi app có namespace NVS riêng, tách khỏi hệ thống và các app khác. Nó bị xóa khi app được gỡ kèm dữ liệu. Không cần quyền.
Mỗi lần ghi xuống flash: đừng gọi trong vòng lặp dày; hãy lưu khi bấm nút hoặc trong `on_close`.

## Phần cứng

Mỗi lời gọi cần quyền ghi bên dưới, nếu không trả `PHONE_ELF_E_PERM` và không làm gì.

| API | SDK | Quyền | Mô tả |
|---|---|---|---|
| `int beep(ctx)` | `hw_beep` | `audio` | Phát một tiếng bíp ngắn ở âm lượng hiện tại; trả về ngay. |
| `int volume_get(ctx)` | | `audio` | Âm lượng loa, phần trăm. |
| `int led_set(ctx, rgb, effect, period_ms)` | `hw_led` | `led` | Hiện một hiệu ứng trên LED của board. Hiệu ứng: `PHONE_ELF_LED_SOLID`, `_BLINK`, `_BREATHE`, `_RAINBOW` (bỏ qua màu); `period_ms` <= 0 nghĩa là 1000. Ưu tiên thấp: nháy đỏ của Ghi âm thắng. |
| `int led_off(ctx)` | `hw_led_off` | `led` | Trả LED lại. Host cũng làm việc này khi app đóng. |
| `int imu_read(ctx, phone_elf_imu_t *out)` | `hw_imu` | `imu` | Mẫu thô gia tốc và con quay (LSM6DS3TR-C): gia tốc +-2 g, 0,061 mg mỗi đơn vị; con quay 250 dps, 8,75 mdps mỗi đơn vị. Cảm biến gắn xoay 90 độ: trục X chỉ xuống cạnh dưới màn hình, trục Y chỉ sang trái. |
| `int battery_read(ctx, phone_elf_battery_t *out)` | `hw_battery` | `battery` | `percent` (-1 khi không có pin), `voltage_mv`, `charging_usb` (có nguồn USB). |
| `int net_online(ctx)` | `hw_online` | `net` | `1` khi điện thoại có Internet (Wi-Fi hoặc 4G), `0` khi không. Chưa có lời gọi mạng nào: app không mở socket được. |

## Vòng đời

```c
typedef struct {
    void (*on_pause)(void *user);    // app xuống nền
    void (*on_resume)(void *user);   // app quay lại
    void (*on_close)(void *user);    // app đóng hẳn: lưu trạng thái ở đây
    void *user;
    uint32_t flags;                  // PHONE_ELF_LC_KEEP_TIMERS
} phone_elf_lifecycle_t;

int  lifecycle_set(ctx, const phone_elf_lifecycle_t *handlers);   // SDK: app_lifecycle(on_pause, on_resume, on_close, user, flags)
bool is_foreground(ctx);                                          // SDK: app_foreground
```

- `lifecycle_set` chép cấu trúc; handler `NULL` được phép. Gọi nó từ `main`.
- **`on_pause`**: về Home, hoặc mở app khác. Màn hình và bộ nhớ vẫn còn. Timer tạm dừng trừ khi `flags` có `PHONE_ELF_LC_KEEP_TIMERS`.
- **`on_resume`**: app về lại phía trước; timer đã dừng chạy tiếp.
- **`on_close`**: nút Đóng (`close(ctx)`), Back, hoặc launcher đóng app cũ nhất. API vẫn dùng được ở đây (`pref_set`, tệp): hãy lưu trạng thái. Nó chạy dưới khóa LVGL nên phải ngắn. Ngay sau đó timer và màn hình bị xóa.
- **Không gửi:** tắt hoặc khóa màn hình không gọi `on_pause`; app vẫn ở phía trước.
- App đã đóng rồi mở lại thì bắt đầu lại từ `main` với biến toàn cục mới.

## Hằng số

```c
#define PHONE_ELF_API_VERSION 3
PHONE_ELF_PERM_LED, _AUDIO, _IMU, _BATTERY, _NET, _FILES            // 1 << 0 ... 1 << 5
PHONE_ELF_EV_CLICKED = 0, _VALUE_CHANGED = 1, _LONG_PRESSED = 2
PHONE_ELF_ALIGN_CENTER = 0, _TOP_MID = 1, _BOTTOM_MID = 2
PHONE_ELF_LED_SOLID = 0, _BLINK = 1, _BREATHE = 2, _RAINBOW = 3
PHONE_ELF_LC_KEEP_TIMERS = 1
```

## Lịch sử phiên bản

| Phiên bản | Thay đổi |
|---|---|
| 3 | Tham số ngữ cảnh trong mọi lời gọi; nhiều app cùng lúc; `size`; mã lỗi; quyền; timer, công tắc, xóa, vị trí, bo góc, ẩn; cài đặt 32 bit trong namespace riêng; `pkg_dir`; vòng đời; `obj_set_font_size`. Phiên bản công bố đầu tiên. |

Phiên bản 1 và 2 là bản thử nội bộ và không được hỗ trợ: gói phải ghi `"api": 3`.
