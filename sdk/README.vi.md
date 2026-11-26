# SDK cho ứng dụng ELF (API v3)

[English](README.md) · Tiếng Việt. Tài liệu đầy đủ: [`docs/elf_apps/vi/`](../docs/elf_apps/vi/README.md) (tổng quan, [tham chiếu API](../docs/elf_apps/vi/api.md), [định dạng gói](../docs/elf_apps/vi/package.md), [xử lý sự cố](../docs/elf_apps/vi/troubleshooting.md)).

Một app là một file ELF được điện thoại nạp vào PSRAM, đóng gói cùng `manifest.json` thành `.bpk`. Chép `.bpk` vào `/packages/` trên thẻ nhớ,
mở **Ứng dụng** trên điện thoại và bấm **Cài**: app hiện trên launcher.

## Dựng

```
cd sdk
make APP=examples/hello        # -> out/hello.bpk
```

Cần toolchain RISC-V của ESP-IDF (`~/.espressif/tools/riscv32-esp-elf/...`, hoặc `make TC=<đường dẫn riscv32-esp-elf>`) và Python 3.
Không cần dự án ESP-IDF.

## Một app

- `main(int argc, char *argv[])`: gọi `phone_init(argc, argv)` trước, dựng màn hình dưới `ui_root()`, rồi trả về. Mọi việc sau đó diễn ra trong
  callback (nút, timer, vòng đời) trên tác vụ LVGL: giữ chúng ngắn, không bao giờ chặn.
- `phone_elf_sdk.h`: các hàm bọc ngắn (`ui_label`, `ui_button`, `timer_start`, `pref_int`...). `phone_elf_api.h` (trong `components/phone_elf_apps/include`) là
  API thật với quy tắc của từng lời gọi, và `PHONE(fn, ...)` với tới mọi thành viên.
- Chỉ các hàm libc sau liên kết được: bộ nhớ, chuỗi, ctype, toán, họ `printf`, tệp (`fopen` ... có sandbox). Danh sách nằm ở
  `components/phone_elf_apps/phone_elf_symbols.c`; ELF cần symbol khác bị từ chối lúc nạp. Không có mạng, không có thread.
- Tệp: `app_data_dir()` (`/sdcard/appdata/<id>`, cần quyền `files`) và `app_pkg_dir()` (gói, chỉ đọc).
- Cài đặt: `pref_int(khóa, mặc_định)` / `pref_save(khóa, giá_trị)`: giá trị 32 bit, khóa tối đa 15 ký tự, riêng cho từng app.
- Vòng đời: `app_lifecycle(on_pause, on_resume, on_close, user, flags)`. Timer tạm dừng ở nền (cờ `PHONE_ELF_LC_KEEP_TIMERS` giữ chúng chạy).
  `on_close` chạy trước khi màn hình bị xóa: hãy lưu trạng thái ở đó.

## manifest.json

```json
{
  "package": { "id": "hello_sdk", "name": { "en": "Hello", "vi": "Xin chào" }, "version": "0.1.0", "icon": "icon.png" },
  "runtime": { "type": "ELF", "entry": "hello.elf", "api": 3 },
  "permissions": ["led", "audio", "imu", "battery", "net", "files"]
}
```

- `id`: chữ, số, `_ - .`, tối đa 32. Hai gói cùng id là cùng một app.
- `runtime.api` bắt buộc và phải bằng phiên bản API của điện thoại: gói dựng cho phiên bản khác bị từ chối kèm thông báo.
- `permissions`: các nhóm lời gọi app dùng (phần còn lại trả `PHONE_ELF_E_PERM`). Quyền chỉ chặn lỗi vô tình, không phải rào bảo vệ trước ELF độc hại:
  chỉ cài gói bạn tin cậy.
- `icon`: PNG cỡ bất kỳ (hiện ở 126 x 126); không có thì điện thoại tự vẽ.
