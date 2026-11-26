# Ứng dụng ELF: định dạng gói (`.bpk`)

[Mục lục](README.md) · [Tổng quan](overview.md) · [Tham chiếu API](api.md) · [Xử lý sự cố](troubleshooting.md) · [English](../package.md)

Một app được phát hành dưới dạng **`.bpk`**: một file ZIP thường với `manifest.json` ở gốc, file ELF mà nó nêu tên, và icon (không bắt buộc).

```
hello.bpk
  manifest.json
  hello.elf
  icon.png          (không bắt buộc)
  res/...           (không bắt buộc: tệp bất kỳ app đọc bằng pkg_dir())
```

## Trên thẻ nhớ

| Đường dẫn | Nội dung |
|---|---|
| `/packages/<tên>.bpk` | gói đang chờ cài (danh sách "Trong kho" của app Ứng dụng) |
| `/userapps/<id>/` | gói đã cài, đã giải nén (danh sách "Đã cài"; nạp lúc khởi động) |
| `/appdata/<id>/` | dữ liệu của app (`app_dir()`); giữ hoặc xóa khi gỡ app |

`/apps/` không dùng: để dành cho app của ESP-Brookesia 0.8.

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

| Trường | Bắt buộc | Ý nghĩa |
|---|---|---|
| `package.id` | có | Id duy nhất: chữ, số, `_`, `-`, `.`, tối đa 32 ký tự. Cũng là tên thư mục trong `/userapps/` và `/appdata/`, namespace cài đặt và chủ của LED. Hai gói cùng id là cùng một app. |
| `package.name` | không | Một chuỗi, hoặc `{ "en": "...", "vi": "..." }`: tên dưới icon, chọn theo ngôn ngữ của điện thoại. Mặc định là id. |
| `package.version` | không | Hiện trong danh sách (văn bản tự do). Chưa so sánh. |
| `package.icon` | không | Đường dẫn PNG trong gói, mặc định `icon.png`. Cỡ bất kỳ, hiện ở 126 x 126 (điểm gần nhất; nên vẽ sẵn 126 x 126). Không có icon thì điện thoại tự vẽ từ id. |
| `runtime.type` | có | Phải là `ELF`. Loại khác bị bỏ qua. |
| `runtime.entry` | có | Đường dẫn ELF trong gói (tương đối, không `..`, không tuyệt đối). |
| `runtime.api` | có | Phiên bản API mà ELF được dựng cho. Phải bằng phiên bản của điện thoại (hiện là **3**), nếu không gói bị từ chối. |
| `permissions` | không | Mảng các nhóm app dùng. Mặc định: không có. |

Trường lạ bị bỏ qua. Tên quyền điện thoại không biết bị bỏ qua và có cảnh báo trong log.

### Quyền

| Tên | Các lời gọi được mở |
|---|---|
| `led` | `led_set`, `led_off` |
| `audio` | `beep`, `volume_get` |
| `imu` | `imu_read` |
| `battery` | `battery_read` |
| `net` | `net_online` |
| `files` | `make_dirs`, và `fopen` / `remove` / `rename` trong `app_dir()` |

Đọc thư mục gói (`pkg_dir()`), cài đặt, giao diện, timer và vòng đời không cần quyền. Xem [overview.md](overview.md#mô-hình-bảo-mật) về việc quyền bảo vệ và không bảo vệ điều gì.

## Giới hạn ZIP

Chỉ mục stored hoặc deflate. Không ZIP64, không mã hóa, không lưu trữ nhiều đĩa. Tối đa 256 mục, 4 MB mỗi tệp, 16 MB sau giải nén, thư mục trung tâm tới 128 KB.
Mục có đường dẫn tuyệt đối, `..` hoặc `\` làm cả gói thất bại. `META-INF/` được bỏ qua (dành cho chữ ký, chưa kiểm).
Mỗi tệp được kiểm CRC-32 khi giải nén.

## Dựng một gói

Bằng SDK (không cần dự án ESP-IDF):

```
cd sdk
make APP=examples/hello          # build/hello.elf, out/hello.bpk
```

Tự đóng một thư mục (ELF đã dựng): `python3 tools/make_bpk.py <thư mục> <ra.bpk>`. Thư mục phải có `manifest.json`.
ELF phải dựng đúng kiểu loader cần: độc lập vị trí, không tệp khởi động, điểm vào `app_main` (`main` được đổi tên), xem [`sdk/Makefile`](../../../sdk/Makefile).

## Cài, cập nhật, gỡ

- **Cài:** chép `.bpk` vào `/packages/` trên thẻ (Eject thẻ đúng cách), mở **Ứng dụng**, bấm **Làm mới**, rồi **Cài**.
  Gói được giải nén vào `/userapps/<id>/`; cài lỗi không để lại gì.
- **Cập nhật:** gỡ app rồi cài `.bpk` mới (cùng id). Muốn giữ dữ liệu thì tắt "Xóa cả dữ liệu khi gỡ" trước khi gỡ.
  Chưa có cập nhật tại chỗ hay so sánh phiên bản.
- **Gỡ:** bấm **Gỡ** hai lần (lần đầu hỏi "Chắc chắn?" trong 3 giây). App đang chạy không gỡ được: đóng nó trước.
  Khi "Xóa cả dữ liệu khi gỡ" bật (mặc định), cài đặt và `/appdata/<id>/` cũng bị xóa.
- **Gói không nạp được:** thư mục trong `/userapps/` không nạp được (ví dụ dựng cho phiên bản API khác) hiện trong "Đã cài" kèm lý do và nút **Xóa**.
  `.bpk` của phiên bản API khác hiện trong "Trong kho" với nút **Cũ** bị tắt.
