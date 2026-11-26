# Ứng dụng ELF

Ứng dụng native nằm trên thẻ nhớ của điện thoại KSDIY ESP32-P4C5: chép một file `.bpk` vào thẻ, cài từ app **Ứng dụng**, và nó hiện trên launcher.
Không build lại, không flash. Ngôn ngữ: Tiếng Việt · [English](../README.md) (bản mặc định).

| Tài liệu | Dùng để |
|---|---|
| [overview.md](overview.md) | tổng quan, các phần ghép với nhau thế nào, vòng đời một app, giới hạn, mô hình bảo mật, cấu hình bắt buộc |
| [api.md](api.md) | tham chiếu API C (phiên bản 3): từng hàm, quyền, vòng đời, quy tắc |
| [package.md](package.md) | định dạng `.bpk`, các trường `manifest.json`, quyền, giới hạn ZIP, cài / cập nhật / gỡ |
| [troubleshooting.md](troubleshooting.md) | triệu chứng, nguyên nhân, cách sửa; đọc log và bản dump khi crash |
| [../../../sdk/README.vi.md](../../../sdk/README.vi.md) | SDK: dựng app và `.bpk` không cần dự án ESP-IDF |

## Bắt đầu nhanh

```
cd sdk
make APP=examples/hello                          # -> out/hello.bpk
cp out/hello.bpk /media/$USER/<thẻ>/packages/    # rồi Eject thẻ
```

Trên điện thoại: **Ứng dụng** -> **Làm mới** -> **Cài** cạnh "Xin chào". Icon mới hiện trên launcher.
Mã nguồn là `sdk/examples/hello/hello.c` (khoảng 70 dòng); app thử dùng mọi hàm là `tools/app_test/main/app_test.c`.

## Mã nguồn nằm ở đâu

| Đường dẫn | Nội dung |
|---|---|
| `components/phone_elf_apps/` | phía firmware: header API, host, danh sách hàm, ElfApp và app Ứng dụng, bộ đọc ZIP |
| `sdk/` | phía app: `phone_elf_sdk.h`, `Makefile`, ví dụ |
| `tools/app_test/` | app thử, dựng các gói mẫu trong `sdcard/packages/` |
| `tools/make_bpk.py`, `tools/make_icons.py` | đóng một thư mục thành `.bpk`; vẽ icon mẫu |
