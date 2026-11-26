# Ứng dụng ELF: tổng quan

[Mục lục](README.md) · [Tham chiếu API](api.md) · [Định dạng gói](package.md) · [Xử lý sự cố](troubleshooting.md) · [English](../overview.md)

Ứng dụng ELF là các app native nhỏ nằm trên thẻ nhớ. Chép gói `.bpk` vào thẻ, cài từ app **Ứng dụng** của điện thoại, và nó hiện trên launcher.
Không có gì được biên dịch vào firmware, không cần flash.

## Bạn có gì

- Tốc độ native: app là mã máy RISC-V nạp vào PSRAM (không có trình thông dịch).
- Một API C nhỏ, có phiên bản, cho giao diện (widget LVGL), timer, cài đặt, tệp và vài dịch vụ phần cứng (LED, loa, IMU, pin, trạng thái mạng).
- Nhiều app chạy cùng lúc, mỗi app có ảnh bộ nhớ, cài đặt và quyền riêng.
- Cài, cập nhật bằng cài lại, và gỡ ngay lúc đang chạy, không cần khởi động lại.

## Các phần ghép với nhau

```
 thẻ nhớ                                       firmware (components/phone_elf_apps)
 -------                                       --------------------------------------------------
 /packages/*.bpk        --cài-->  app Ứng dụng (ElfAppManager)   giải nén, kiểm manifest.json, liệt kê, gỡ
 /userapps/<id>/                  |  quét lúc khởi động + lúc cài
   manifest.json, app.elf, icon.png  v
 /appdata/<id>/   (dữ liệu của app) ElfApp (mỗi gói một cái)     icon launcher, vòng đời: run / pause / resume / close
                                  |
                                  v
                                  host (phone_elf_host.c)         mỗi app đang chạy một instance: ảnh ELF, ngữ cảnh,
                                  |                               quyền, timer, handler sự kiện, namespace NVS
                                  v
                                  loader (espressif/elf_loader)   dời (relocate) ELF vào PSRAM; symbol = phone_elf_symbols.c
```

| Phần | Tệp | Việc |
|---|---|---|
| Bảng API | `include/phone_elf_api.h` | Hợp đồng giữa firmware và app (phiên bản 3). |
| Host | `phone_elf_host.c` | Nạp một ELF, dựng màn hình của nó, cài đặt mọi lời gọi API, kiểm quyền, chạy các handler. Tối đa 4 instance. |
| Danh sách symbol | `phone_elf_symbols.c` | Các hàm libc duy nhất mà app liên kết được; `fopen`, `remove`, `rename` có sandbox. |
| ElfApp, app Ứng dụng | `phone_elf_apps.cpp` | Mỗi gói đã cài một `ElfApp` (mục launcher, vòng đời). App Ứng dụng liệt kê, cài và gỡ gói. |
| Bộ đọc ZIP | `phone_zip.c` | Đọc file `.bpk` (ZIP, stored hoặc deflate). |
| SDK | `sdk/` | `phone_elf_sdk.h`, Makefile độc lập, một app ví dụ. |

## Vòng đời một app

1. **Cài.** App Ứng dụng đọc `manifest.json` trong `/packages/<tên>.bpk`, kiểm tra (`runtime.api`, id), giải nén gói vào
   `/userapps/<id>/` và đăng ký một mục launcher. Không cần khởi động lại.
2. **Mở.** Brookesia gọi `ElfApp::run()`: host đọc ELF, dời nó vào PSRAM (vài ms), tạo vùng chứa toàn màn hình
   (`root()`) và gọi `main(argc, argv)` với bảng API và ngữ cảnh của instance này. `main` dựng màn hình rồi trả về.
3. **Chạy.** Từ đó app sống trong các callback: sự kiện, timer, handler vòng đời. Tất cả chạy trên tác vụ LVGL.
4. **Nền.** Về Home hoặc mở app khác thì gọi `on_pause`; màn hình vẫn còn, timer tạm dừng (trừ khi app xin giữ).
   Quay lại thì gọi `on_resume`.
5. **Đóng.** Nút Đóng, Back, hoặc launcher đóng app cũ nhất thì gọi `on_close` (màn hình còn: lưu trạng thái), rồi host
   xóa timer và màn hình và gỡ ELF khỏi bộ nhớ.
6. **Gỡ.** App Ứng dụng xóa `/userapps/<id>/` và, nếu công tắc "Xóa cả dữ liệu khi gỡ" bật, cả cài đặt (namespace NVS) và `/appdata/<id>/`.

Khi khởi động, điện thoại quét `/userapps/` và đăng ký mọi gói nạp được. Gói không nạp được (sai phiên bản API, manifest hỏng)
được liệt kê trong app Ứng dụng kèm lý do và nút **Xóa**.

## Bộ nhớ và giới hạn

| Mục | Giá trị |
|---|---|
| Số app ELF nạp cùng lúc | 4 (`PHONE_ELF_HOST_MAX_APPS`); Brookesia còn đóng app cũ nhất khi đủ 3 app chạy |
| Bộ nhớ của một app | ảnh ELF trong PSRAM (vài KB), các `malloc` trong PSRAM, khoảng 63 KB PSRAM cho icon (dùng chung theo id) |
| Timer mỗi app | 8 |
| Handler sự kiện mỗi app | 64 (mỗi lần gọi `obj_add_event_cb` một cái) |
| Khóa cài đặt | tối đa 15 ký tự, giá trị 32 bit |
| Gói | 256 tệp, 4 MB mỗi tệp, 16 MB sau giải nén, `manifest.json` tối đa 16 KB, icon tối đa 256 KB |

Chưa có hạn mức bộ nhớ cho từng app: một app có thể `malloc` cho tới khi hết PSRAM.

## Mô hình bảo mật

Các cơ chế dưới đây chặn **lỗi vô tình và lạm dụng thông thường**. Chúng không chặn được một ELF có ý đồ xấu.

- **Quyền** (`"permissions"` trong `manifest.json`): lời gọi thuộc nhóm manifest không xin sẽ lỗi với `PHONE_ELF_E_PERM`.
- **Danh sách symbol**: loader không xuất gì của riêng nó (`CONFIG_ELF_LOADER_LIBC_SYMBOLS` và `CONFIG_ELF_LOADER_ESPIDF_SYMBOLS` tắt). App chỉ liên kết được
  với `phone_elf_symbols.c`: chuỗi, bộ nhớ, toán, `printf` và tệp. Không có socket, NVS, reset, thread. ELF cần thứ khác sẽ bị từ chối lúc nạp.
- **Sandbox tệp**: `fopen`, `remove`, `rename` chỉ với tới `/sdcard/appdata/<id>/` (có quyền `files`) và thư mục gói (chỉ đọc).
- **Cài đặt riêng**: mỗi app một namespace NVS.

Những gì **không** được bảo vệ: không có MMU, và firmware chạy với `CONFIG_ESP_SYSTEM_MEMPROT` tắt (bảo vệ bộ nhớ PMP đặt heap PSRAM không cho thực thi,
nên ELF không chạy được ở đó). ELF có thể đọc và ghi mọi vùng nhớ và nhảy tới mọi địa chỉ, kể cả bộ nhớ của app khác và của firmware.
Chưa kiểm chữ ký gói. **Chỉ cài gói bạn tin cậy.**

## Cấu hình bắt buộc

Đặt trong `sdkconfig.defaults` của dự án phone:

| Tùy chọn | Giá trị | Vì sao |
|---|---|---|
| `CONFIG_ESP_SYSTEM_MEMPROT` | tắt | mã nạp vào PSRAM phải thực thi được |
| `CONFIG_ELF_LOADER_LIBC_SYMBOLS`, `CONFIG_ELF_LOADER_ESPIDF_SYMBOLS` | tắt | app chỉ dùng được danh sách symbol chọn lọc |
| `CONFIG_LV_USE_LODEPNG` | bật | icon PNG trong gói |
| `CONFIG_ELF_LOADER_LOAD_PSRAM` | bật (mặc định khi có SPIRAM) | nạp vào PSRAM |

## Quan hệ với ESP-Brookesia 0.8

Cấu trúc gói (`manifest.json` với các trường `package` / `runtime`) theo gói app của ESP-Brookesia 0.8 để sau này có thể dùng chung công cụ và gói.
Điện thoại **không** dùng framework Brookesia 0.8: nó chạy `brookesia_core` cũ hơn, và runtime ELF của Brookesia 0.8 chỉ nạp ELF rồi gọi nó
(không có API host). Host, API, app Ứng dụng và bộ đọc `.bpk` ở đây là do chúng ta tự viết. `runtime.api` và `permissions` là phần chúng ta thêm.

## Hiện trạng

Chạy được trên board: cài, mở, nhiều app cùng lúc, vòng đời, quyền, tệp có sandbox, cài đặt riêng mỗi app, gỡ kèm xóa dữ liệu, `.bpk` có icon, SDK độc lập.
Chưa làm (xem [package.md](package.md) và nhật ký thiết kế `PLAN_EXT_APPS.md`): chữ ký gói, cập nhật phiên bản cùng id, tự quét lại thẻ,
`pause` khi tắt màn hình, nhiều widget hơn, âm thanh, HTTP, hạn mức bộ nhớ mỗi app.
