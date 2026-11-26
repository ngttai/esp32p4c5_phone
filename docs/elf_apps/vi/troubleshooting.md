# Ứng dụng ELF: xử lý sự cố

[Mục lục](README.md) · [Tổng quan](overview.md) · [Tham chiếu API](api.md) · [Định dạng gói](package.md) · [English](../troubleshooting.md)

Điện thoại ghi một dòng log cho từng bước. Cắm cổng USB debug và đọc console (tag `ELFAPPS`, `ELFHOST`, `ELFSYM`).
Mở cổng serial làm board khởi động lại: hãy bật bộ ghi log trước, rồi mới thực hiện các bước.

| Triệu chứng | Nguyên nhân có thể | Cách xử lý |
|---|---|---|
| `.bpk` không có trong "Trong kho" | Không nằm trong `/packages/`, chưa Eject thẻ trước khi rút, hoặc ZIP không có `manifest.json` ở gốc | Kiểm tra đường dẫn; Eject thẻ trên PC; bấm **Làm mới**. Log có `<tệp>: ...` (ví dụ `file not in the archive`, `not a ZIP file`). |
| `.bpk` có ghi chú đỏ và nút **Cũ** bị tắt | `runtime.api` thiếu hoặc không phải phiên bản API của điện thoại | Dựng lại app theo `phone_elf_api.h` hiện tại và đặt `"api": 3`. |
| **Cài** báo `a folder with this id is already in userapps` | Thư mục cũ cùng id nằm trong `/userapps/` | Gỡ app đã cài (hoặc **Xóa** gói hỏng), rồi cài lại. |
| **Cài** báo `already installed` | Id đã được cài | Gỡ nó trước. |
| Gói trong "Đã cài" có ghi chú và nút **Xóa** | Điện thoại không nạp được: phiên bản API khác, `manifest.json` không đọc được, trùng id | Đọc ghi chú; **Xóa** nó, hoặc sửa gói rồi cài lại. |
| Icon app hiện nhưng mở ra chữ đỏ `cannot load the ELF (symbol or format)` | ELF cần symbol không có trong danh sách, hoặc dựng cho kiến trúc khác hay thiếu `-fPIC` | Tìm `Can't find common <tên>` trong log. Chỉ dùng thứ `phone_elf_symbols.c` liệt kê, hoặc thêm hàm vào đó. Dựng với cờ của `sdk/Makefile`. |
| Mở ra `cannot read the ELF file` | `runtime.entry` nêu tên tệp không có trong gói | Sửa `entry`; đóng lại gói. |
| Mở ra `too many ELF apps are open` | Đã nạp 4 app ELF | Đóng bớt một app. |
| Màn hình app trống và log ghi `entry returned -1` | `main` trả lỗi trước khi dựng màn hình: `phone_init` thất bại (khác phiên bản API) hoặc `argc < 2` | Dựng theo đúng `phone_elf_api.h` của firmware. |
| Một lời gọi không làm gì và log ghi `permission not in the manifest` | App dùng nhóm không liệt kê trong `permissions` | Thêm nhóm vào `manifest.json`, đóng gói lại, cài lại. Lời gọi trả `PHONE_ELF_E_PERM` (-1). |
| `fopen` trả `NULL` và log ghi `refused` | Đường dẫn ngoài `app_dir()` / `pkg_dir()`, hoặc đang ghi vào `pkg_dir()`, hoặc app thiếu `files` | Dùng `path_in_data()` / `app_dir()`; thêm `"files"`; gọi `make_dirs` trước lần ghi đầu. |
| `fopen` chạy nhưng tệp mất sau khi cài lại | App được gỡ khi "Xóa cả dữ liệu khi gỡ" đang bật | Tắt công tắc trước khi gỡ nếu cần giữ dữ liệu. |
| `pref_set` trả -22 | Khóa rỗng, hoặc dài quá 15 ký tự | Rút ngắn khóa. |
| Chữ tiếng Việt hiện thành ô vuông | Đã đặt font riêng, hoặc chữ vẽ ngoài vùng chứa của host | Dùng font mặc định; vùng chứa dùng font có đủ dấu tiếng Việt. |
| Màn hình đứng một lúc | Một callback bị chặn (`delay_ms`, vòng lặp dài, ghi tệp lớn) | Giữ callback ngắn; làm việc theo từng bước nhỏ từ một timer. |
| **Gỡ** báo `close the app first` | App đang mở (có thể ở nền) | Đóng nó từ màn hình recents, rồi gỡ. |
| Sau một lần crash điện thoại khởi động lại | ELF ghi vào bộ nhớ không phải của nó, hoặc chạy lệnh không hợp lệ | Đọc bản dump panic trong log. Không có cách ly giữa các app và firmware: hãy thử app mới trên board phát triển. |
| **Cài** lỗi với `the launcher refused it` hoặc `out of memory` | Launcher không nhận thêm app, hoặc thiếu bộ nhớ | Đóng bớt app và gỡ những app không dùng; thử lại. |

## Đọc bản dump panic

Một lần crash in `Guru Meditation Error` kèm `MEPC` (địa chỉ gây lỗi). Địa chỉ từ `0x48f...` đến `0x490...` (PSRAM) nằm trong ảnh một ELF; dòng log
`ELFHOST [<id>] ... base 0x48f..` lúc nạp cho biết cơ sở của app đó.
Để tìm hàm, dựng app với `-g` (giữ ELF chưa strip) và tra độ lệch `MEPC - base` bằng công cụ của loader
`managed_components/espressif__elf_loader/tool/elf_addr2line.py` (hoặc `riscv32-esp-elf-addr2line`): ELF độc lập vị trí và được liên kết từ địa chỉ 0.

## Các dòng log hữu ích

| Dòng | Ý nghĩa |
|---|---|
| `ELFAPPS: installed 'Tên' (id) as app N, permissions 0x..` | Gói đã được đăng ký lên launcher. |
| `ELFHOST: [id] N bytes, relocate 0, T us, base 0x.., perms 0x..` | ELF đã được nạp (relocate 0 = thành công). |
| `ELFHOST: [id] entry returned 0` | `main` đã xong; app giờ sống trong các callback. |
| `ELFHOST: [id] pause` / `resume` / `closing` / `closed` | Vòng đời. `closed` cho thấy PSRAM và SRAM trống sau khi gỡ. |
| `ELFSYM: [id] fopen(path, mode) refused` | Sandbox tệp từ chối một đường dẫn. |
| `ELFHOST: [id] <lời gọi>: permission not in the manifest` | Một lời gọi thiếu quyền. |
| `ELFAPPS: uninstalled id and its data` / `, data kept` | App đã được gỡ. |
