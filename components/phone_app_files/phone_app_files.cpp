/*
 * Files app: browse the microSD card. A tap opens a folder, shows a text file or the details of
 * another file; a long press shows the details of any entry. Files and empty folders can be
 * deleted. The USB button lends the card to a PC as a USB drive (mass storage on the USB HS
 * Type-C socket) until it is turned off or the app is left.
 */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "esp_blockdev.h"
#include "sdmmc_cmd.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "FILES"
#include "esp_lib_utils.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_storage.h"
#include "phone_app_files.hpp"

#define APP_NAME "Tệp"

extern "C" const lv_image_dsc_t phone_files_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_files";

constexpr const char *ROOT = "/sdcard";
constexpr int MAX_ENTRIES = 300;
constexpr int HEADER_H = 64;
constexpr int FOOTER_H = 64;        // storage line + the navigation gesture strip

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_ROW = 0x1E252C;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_FOLDER = 0xFFD65A;
constexpr uint32_t COLOR_DANGER = 0xE5484D;
constexpr uint32_t COLOR_USB = 0x2F6FED;
constexpr size_t TEXT_MAX = 32 * 1024;      // text preview limit
constexpr int NAV_STRIP_H = 36;             // swipe-up strip of the navigation bar

struct Entry {
    std::string name;
    bool is_dir;
    uint32_t size;
};

lv_obj_t *s_root;
lv_obj_t *s_path;
lv_obj_t *s_list;
lv_obj_t *s_storage;
lv_obj_t *s_sheet;          // details of one entry, over the list
lv_obj_t *s_sheet_info;
lv_obj_t *s_sheet_delete;
lv_obj_t *s_text;           // text preview, over the list
lv_obj_t *s_text_title;
lv_obj_t *s_text_body;
lv_obj_t *s_usb;            // USB drive panel, over everything
lv_obj_t *s_usb_state;
lv_obj_t *s_usb_msg;        // why the USB drive could not start
lv_timer_t *s_timer;
std::string s_dir;
std::vector<Entry> s_entries;
int s_selected = -1;
uint32_t s_delete_armed_until;
uint32_t s_usb_msg_until;

bool s_usb_on;
/* The card as a block device for the USB drive. Writes go one sector at a time (single block
 * write, as MetalioClaw4 does with a 512-byte MSC buffer): the stop command that ends a multiple
 * block write breaks during long write bursts on this board, whose SDMMC host also runs the SDIO
 * link of the Wi-Fi chip, and leaves the card in the receive state. Reads stay multiple block.
 * A failed transfer re-initializes the card and is tried again: the writes of the PC are already
 * acknowledged when they reach the card, so they must not be dropped. */
struct UsbDisk {
    esp_blockdev_t dev;
    sdmmc_card_t *card;
    uint32_t recoveries;
};
UsbDisk s_disk;
bool s_msc_driver;
tinyusb_msc_storage_handle_t s_msc_storage;

void format_size(char *buf, size_t len, uint64_t bytes)
{
    if (bytes < 1024) {
        snprintf(buf, len, "%u B", static_cast<unsigned>(bytes));
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, len, "%u KB", static_cast<unsigned>(bytes / 1024));
    } else if (bytes < 1024ULL * 1024 * 1024) {
        const unsigned tenths = static_cast<unsigned>(bytes * 10 / (1024 * 1024));
        snprintf(buf, len, "%u.%u MB", tenths / 10, tenths % 10);
    } else {
        const unsigned tenths = static_cast<unsigned>(bytes * 10 / (1024ULL * 1024 * 1024));
        snprintf(buf, len, "%u.%u GB", tenths / 10, tenths % 10);
    }
}

const char *icon_for(const Entry &e)
{
    if (e.is_dir) {
        return LV_SYMBOL_DIRECTORY;
    }
    const char *dot = strrchr(e.name.c_str(), '.');
    if (dot) {
        for (const char *ext : {".jpg", ".jpeg", ".png", ".bmp", ".gif"}) {
            if (strcasecmp(dot, ext) == 0) {
                return LV_SYMBOL_IMAGE;
            }
        }
        for (const char *ext : {".mp3", ".wav", ".flac", ".aac", ".m4a", ".ogg"}) {
            if (strcasecmp(dot, ext) == 0) {
                return LV_SYMBOL_AUDIO;
            }
        }
        for (const char *ext : {".mp4", ".avi", ".mjpeg", ".mkv"}) {
            if (strcasecmp(dot, ext) == 0) {
                return LV_SYMBOL_VIDEO;
            }
        }
    }
    return LV_SYMBOL_FILE;
}

std::string full_path(const std::string &name)
{
    return s_dir + "/" + name;
}

void scan()
{
    s_entries.clear();
    DIR *dir = opendir(s_dir.c_str());
    if (!dir) {
        return;
    }
    while (struct dirent *e = readdir(dir)) {
        if (static_cast<int>(s_entries.size()) >= MAX_ENTRIES) {
            break;
        }
        Entry en = {};
        en.name = e->d_name;
        en.is_dir = e->d_type == DT_DIR;
        if (!en.is_dir) {
            struct stat st;
            if (stat(full_path(en.name).c_str(), &st) == 0) {
                en.size = st.st_size;
            }
        }
        s_entries.push_back(std::move(en));
    }
    closedir(dir);
    /* folders first, then by name without regard to case */
    std::sort(s_entries.begin(), s_entries.end(), [](const Entry & a, const Entry & b) {
        if (a.is_dir != b.is_dir) {
            return a.is_dir;
        }
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

void show_dir();

/* ---- text preview ---- */

bool is_text(const std::string &name)
{
    const char *dot = strrchr(name.c_str(), '.');
    if (!dot) {
        return false;
    }
    for (const char *ext : {".txt", ".json", ".md", ".log", ".csv", ".ini", ".cfg", ".conf", ".xml", ".yml",
                            ".yaml", ".m3u", ".m3u8", ".srt", ".lrc", ".htm", ".html"
                           }) {
        if (strcasecmp(dot, ext) == 0) {
            return true;
        }
    }
    return false;
}

/* Keeps valid UTF-8 and line breaks; other bytes become '?' so the label never sees broken text */
void clean_text(std::string &t)
{
    std::string out;
    out.reserve(t.size());
    const auto *p = reinterpret_cast<const unsigned char *>(t.data());
    const size_t n = t.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        if (c == '\r') {
            i++;
            continue;
        }
        if (c < 0x80) {
            out += (c < 0x20 && c != '\n' && c != '\t') ? ' ' : static_cast<char>(c);
            i++;
            continue;
        }
        const size_t len = c >= 0xF0 && c < 0xF5 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
        bool ok = len && i + len <= n;
        for (size_t k = 1; ok && k < len; k++) {
            ok = (p[i + k] & 0xC0) == 0x80;
        }
        if (ok) {
            out.append(t, i, len);
            i += len;
        } else {
            out += '?';
            i++;
        }
    }
    t.swap(out);
}

void close_text()
{
    if (s_text) {
        lv_obj_add_flag(s_text, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_text_body, "");
    }
}

bool text_open()
{
    return s_text && !lv_obj_has_flag(s_text, LV_OBJ_FLAG_HIDDEN);
}

void open_text(const Entry &e)
{
    std::string t;
    FILE *f = fopen(full_path(e.name).c_str(), "rb");
    if (f) {
        t.resize(std::min<size_t>(e.size, TEXT_MAX));
        t.resize(fread(t.data(), 1, t.size(), f));
        fclose(f);
    }
    clean_text(t);
    if (!f) {
        t = T("Không mở được tệp", "Cannot open the file");
    } else if (e.size > TEXT_MAX) {
        t += T("\n\n[Chỉ hiện 32 KB đầu]", "\n\n[First 32 KB only]");
    } else if (t.empty()) {
        t = T("(Tệp trống)", "(Empty file)");
    }
    lv_label_set_text(s_text_title, e.name.c_str());
    lv_label_set_text(s_text_body, t.c_str());
    lv_obj_scroll_to_y(lv_obj_get_parent(s_text_body), 0, LV_ANIM_OFF);
    lv_obj_remove_flag(s_text, LV_OBJ_FLAG_HIDDEN);
}

/* ---- USB drive ---- */

enum { ITF_MSC, ITF_TOTAL };
constexpr uint8_t EP_MSC = 0x01;
constexpr int MSC_CONFIG_LEN = TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN;

const uint8_t MSC_CONFIG_HS[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, MSC_CONFIG_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 4, EP_MSC, 0x80 | EP_MSC, 512),
};

const uint8_t MSC_CONFIG_FS[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, MSC_CONFIG_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 4, EP_MSC, 0x80 | EP_MSC, 64),
};

const tusb_desc_device_t MSC_DEVICE = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceProtocol = TUSB_CLASS_UNSPECIFIED,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const tusb_desc_device_qualifier_t MSC_QUALIFIER = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceProtocol = TUSB_CLASS_UNSPECIFIED,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};

const char *MSC_STRINGS[] = {
    (const char[]){0x09, 0x04},
    "Espressif",
    "KSDIY P4C5 microSD",
    "000001",
    "microSD",
};

constexpr int DISK_TRIES = 4;

bool disk_recover(UsbDisk *d)
{
    d->recoveries++;
    const sdmmc_host_t host = d->card->host;    /* sdmmc_card_init clears the card first */
    for (int i = 0; i < 3; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const esp_err_t err = sdmmc_card_init(&host, d->card);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "card re-initialized (%u)", static_cast<unsigned>(d->recoveries));
            return true;
        }
        ESP_LOGE(TAG, "card re-init: %s", esp_err_to_name(err));
    }
    return false;
}

esp_err_t disk_read(esp_blockdev_handle_t h, uint8_t *dst, size_t, uint64_t addr, size_t len)
{
    auto *d = static_cast<UsbDisk *>(h->ctx);
    const size_t ss = d->card->csd.sector_size;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < DISK_TRIES; i++) {
        err = sdmmc_read_sectors(d->card, dst, addr / ss, len / ss);
        if (err == ESP_OK || !disk_recover(d)) {
            break;
        }
    }
    return err;
}

esp_err_t disk_write(esp_blockdev_handle_t h, const uint8_t *src, uint64_t addr, size_t len)
{
    auto *d = static_cast<UsbDisk *>(h->ctx);
    const size_t ss = d->card->csd.sector_size;
    esp_err_t err = ESP_FAIL;
    for (size_t off = 0; off < len; off += ss) {
        for (int i = 0; i < DISK_TRIES; i++) {
            err = sdmmc_write_sectors(d->card, src + off, (addr + off) / ss, 1);
            if (err == ESP_OK || !disk_recover(d)) {
                break;
            }
        }
        if (err != ESP_OK) {
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write of %u bytes at 0x%llx lost", static_cast<unsigned>(len), static_cast<unsigned long long>(addr));
    }
    return err;
}

esp_err_t disk_nop(esp_blockdev_handle_t)
{
    return ESP_OK;
}

const esp_blockdev_ops_t DISK_OPS = {
    .read = disk_read,
    .write = disk_write,
    .erase = nullptr,
    .sync = disk_nop,
    .ioctl = nullptr,
    .release = disk_nop,
};

void show_usb_msg(const char *text)
{
    lv_label_set_text(s_usb_msg, text);
    lv_obj_remove_flag(s_usb_msg, LV_OBJ_FLAG_HIDDEN);
    s_usb_msg_until = lv_tick_get() + 4000;
}

void usb_stop()
{
    if (!s_usb_on) {
        return;
    }
    tinyusb_driver_uninstall();
    if (s_msc_storage) {
        /* writes of the PC still queued: give them a moment */
        for (int i = 0; i < 20 && tinyusb_msc_delete_storage(s_msc_storage) == ESP_ERR_INVALID_STATE; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        s_msc_storage = nullptr;
    }
    if (s_msc_driver) {
        tinyusb_msc_uninstall_driver();
        s_msc_driver = false;
    }
    phone_storage_take_back();
    s_usb_on = false;
    if (s_usb) {
        lv_obj_add_flag(s_usb, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "USB drive off (card re-initialized %u times)", static_cast<unsigned>(s_disk.recoveries));
}

bool usb_start()
{
    char holders[96];
    if (phone_storage_holders(holders, sizeof(holders)) > 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s %s", T("Đóng app trước:", "Close these apps first:"), holders);
        show_usb_msg(msg);
        return false;
    }
    if (phone_storage_get(nullptr, nullptr) != PHONE_STORAGE_MOUNTED) {
        show_usb_msg(T("Không có thẻ nhớ", "No microSD card"));
        return false;
    }
    sdmmc_card_t *card = nullptr;
    if (phone_storage_lend(&card) != ESP_OK) {
        show_usb_msg(T("Không mở được thẻ nhớ", "Could not open the card"));
        return false;
    }
    s_usb_on = true;

    tinyusb_msc_driver_config_t drv = {};
    drv.user_flags.auto_mount_off = 1;      /* the card stays with the PC until turned off here */
    esp_err_t err = tinyusb_msc_install_driver(&drv);
    if (err == ESP_OK) {
        s_msc_driver = true;
        s_disk = {};
        s_disk.card = card;
        s_disk.dev.ctx = &s_disk;
        s_disk.dev.ops = &DISK_OPS;
        const size_t ss = card->csd.sector_size;
        s_disk.dev.geometry.disk_size = static_cast<uint64_t>(card->csd.capacity) * ss;
        s_disk.dev.geometry.read_size = s_disk.dev.geometry.write_size = s_disk.dev.geometry.erase_size = ss;
        s_disk.dev.geometry.recommended_read_size = s_disk.dev.geometry.recommended_write_size = ss;
        s_disk.dev.geometry.recommended_erase_size = ss;
        tinyusb_msc_storage_config_t st = {};
        st.medium.blockdev = &s_disk.dev;
        st.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
        st.fat_fs.do_not_format = true;
        st.fat_fs.config.format_if_mount_failed = false;
        err = tinyusb_msc_new_storage_blockdev(&st, &s_msc_storage);
    }
    if (err == ESP_OK) {
        tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
        cfg.descriptor.device = &MSC_DEVICE;
        cfg.descriptor.qualifier = &MSC_QUALIFIER;
        cfg.descriptor.string = MSC_STRINGS;
        cfg.descriptor.string_count = sizeof(MSC_STRINGS) / sizeof(MSC_STRINGS[0]);
        cfg.descriptor.full_speed_config = MSC_CONFIG_FS;
        cfg.descriptor.high_speed_config = MSC_CONFIG_HS;
        err = tinyusb_driver_install(&cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB drive: %s", esp_err_to_name(err));
        usb_stop();
        show_dir();
        show_usb_msg(T("Không khởi động được USB (cổng USB đang bận?)", "Could not start USB (port busy?)"));
        return false;
    }
    ESP_LOGI(TAG, "USB drive on");
    lv_obj_remove_flag(s_usb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_usb);
    return true;
}

void on_usb(lv_event_t *)
{
    if (!s_usb_on) {
        close_text();
        usb_start();
    }
}

void on_usb_off(lv_event_t *)
{
    usb_stop();
    show_dir();
}

void disarm_delete()
{
    s_delete_armed_until = 0;
    lv_obj_set_style_bg_color(s_sheet_delete, lv_color_hex(0x2A333C), 0);
    lv_label_set_text(lv_obj_get_child(s_sheet_delete, 0), T("Xóa", "Delete"));
}

void close_sheet()
{
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
    s_selected = -1;
}

void open_sheet(int index)
{
    const Entry &e = s_entries[index];
    s_selected = index;
    disarm_delete();
    char size[24] = "";
    char when[24] = "";
    struct stat st;
    if (stat(full_path(e.name).c_str(), &st) == 0) {
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(when, sizeof(when), "%d/%m/%Y %H:%M", &tm);
    }
    if (e.is_dir) {
        lv_label_set_text_fmt(s_sheet_info, "%s\n%s\n%s", e.name.c_str(), T("Thư mục", "Folder"), when);
    } else {
        format_size(size, sizeof(size), e.size);
        lv_label_set_text_fmt(s_sheet_info, "%s\n%s\n%s", e.name.c_str(), size, when);
    }
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
}

void on_row(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= static_cast<int>(s_entries.size())) {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        open_sheet(index);
        return;
    }
    if (s_entries[index].is_dir) {
        s_dir = full_path(s_entries[index].name);
        show_dir();
    } else if (is_text(s_entries[index].name)) {
        open_text(s_entries[index]);
    } else {
        open_sheet(index);
    }
}

/* First tap arms the button for 3 s, the second one deletes */
void on_delete(lv_event_t *)
{
    if (s_selected < 0) {
        return;
    }
    if (lv_tick_get() >= s_delete_armed_until) {
        s_delete_armed_until = lv_tick_get() + 3000;
        lv_obj_set_style_bg_color(s_sheet_delete, lv_color_hex(COLOR_DANGER), 0);
        lv_label_set_text(lv_obj_get_child(s_sheet_delete, 0), T("Xóa thật?", "Really delete?"));
        return;
    }
    const Entry &e = s_entries[s_selected];
    const std::string path = full_path(e.name);
    const int rc = e.is_dir ? rmdir(path.c_str()) : unlink(path.c_str());
    if (rc != 0) {
        ESP_LOGW(TAG, "cannot delete %s", path.c_str());
        disarm_delete();
        lv_label_set_text(s_sheet_info, e.is_dir ? T("Không xóa được: thư mục chưa trống", "Cannot delete: the folder is not empty")
                          : T("Không xóa được tệp", "Cannot delete the file"));
        return;
    }
    close_sheet();
    show_dir();
}

void on_sheet_close(lv_event_t *)
{
    close_sheet();
}

void on_up(lv_event_t *)
{
    if (s_dir.size() > strlen(ROOT)) {
        s_dir.erase(s_dir.rfind('/'));
        show_dir();
    }
}

void show_dir()
{
    lv_obj_clean(s_list);
    const phone_storage_state_t state = phone_storage_get(nullptr, nullptr);
    if (state != PHONE_STORAGE_MOUNTED) {
        s_entries.clear();
        lv_label_set_text(s_path, T("Thẻ nhớ", "microSD card"));
        lv_obj_t *l = lv_label_create(s_list);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_pad_top(l, 120, 0);
        lv_label_set_text(l, state == PHONE_STORAGE_CHECKING ? T("Đang kiểm tra thẻ nhớ...", "Checking the card...")
                          : state == PHONE_STORAGE_USB ? T("Thẻ nhớ đang mở trên máy tính", "The card is open on the PC")
                          : T("Không có thẻ nhớ", "No microSD card"));
        lv_label_set_text(s_storage, "");
        return;
    }
    scan();
    lv_label_set_text(s_path, s_dir.size() > strlen(ROOT) ? s_dir.c_str() + strlen(ROOT) : "/");
    for (size_t i = 0; i < s_entries.size(); i++) {
        const Entry &e = s_entries[i];
        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3A444E), LV_STATE_PRESSED);
        void *user = reinterpret_cast<void *>(static_cast<intptr_t>(i));
        lv_obj_add_event_cb(row, on_row, LV_EVENT_SHORT_CLICKED, user);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_LONG_PRESSED, user);

        lv_obj_t *icon = lv_label_create(row);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(e.is_dir ? COLOR_FOLDER : COLOR_SUB), 0);
        lv_label_set_text(icon, icon_for(e));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t *size = nullptr;
        if (!e.is_dir) {
            char buf[24];
            format_size(buf, sizeof(buf), e.size);
            size = lv_label_create(row);
            lv_obj_set_style_text_color(size, lv_color_hex(COLOR_SUB), 0);
            lv_obj_set_style_text_font(size, phone_font_by_size(16), 0);
            lv_label_set_text(size, buf);
            lv_obj_align(size, LV_ALIGN_RIGHT_MID, -14, 0);
        }
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, e.is_dir ? 380 : 290);
        lv_obj_set_height(name, lv_font_get_line_height(phone_font_by_size(20)));
        lv_label_set_text(name, e.name.c_str());
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 54, 0);
    }
    if (s_entries.empty()) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_pad_top(l, 120, 0);
        lv_label_set_text(l, T("Thư mục trống", "Empty folder"));
    }
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);

    uint64_t total = 0, free = 0;
    phone_storage_get(&total, &free);
    char a[24], b[24];
    format_size(a, sizeof(a), free);
    format_size(b, sizeof(b), total);
    lv_label_set_text_fmt(s_storage, "%s %s / %s", T("Còn trống", "Free"), a, b);
}

void on_timer(lv_timer_t *)
{
    if (!s_root) {
        return;
    }
    if (s_delete_armed_until && lv_tick_get() >= s_delete_armed_until) {
        disarm_delete();
    }
    if (s_usb_msg_until && lv_tick_get() >= s_usb_msg_until) {
        s_usb_msg_until = 0;
        lv_obj_add_flag(s_usb_msg, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_usb_on) {
        lv_label_set_text(s_usb_state, tud_mounted() ? T("Máy tính đã nhận ổ", "Connected to the PC")
                          : T("Đang chờ máy tính...", "Waiting for the PC..."));
        lv_display_trigger_activity(nullptr);       /* in use: keep the screen on */
    }
}

lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 52);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A333C), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

void build_ui()
{
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(root, lv_color_white(), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(20), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    const int root_w = lv_obj_get_width(root);
    const int root_h = lv_obj_get_height(root);

    /* header: up button and the current path */
    lv_obj_t *up = lv_button_create(root);
    lv_obj_remove_style_all(up);
    lv_obj_set_size(up, 48, 44);
    lv_obj_set_style_radius(up, 22, 0);
    lv_obj_set_style_bg_opa(up, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(up, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_color(up, lv_color_hex(0x3A444E), LV_STATE_PRESSED);
    lv_obj_align(up, LV_ALIGN_TOP_LEFT, 12, 10);
    lv_obj_add_event_cb(up, on_up, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ul = lv_label_create(up);
    lv_label_set_text(ul, LV_SYMBOL_UP);
    lv_obj_center(ul);

    s_path = lv_label_create(root);
    lv_label_set_long_mode(s_path, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_path, root_w - 148);
    lv_obj_set_height(s_path, lv_font_get_line_height(phone_font_by_size(20)));
    lv_obj_align(s_path, LV_ALIGN_TOP_LEFT, 72, 20);
    lv_label_set_text(s_path, "/");

    lv_obj_t *usb = lv_button_create(root);
    lv_obj_remove_style_all(usb);
    lv_obj_set_size(usb, 56, 44);
    lv_obj_set_style_radius(usb, 22, 0);
    lv_obj_set_style_bg_opa(usb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(usb, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_color(usb, lv_color_hex(0x3A444E), LV_STATE_PRESSED);
    lv_obj_align(usb, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_add_event_cb(usb, on_usb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *usb_l = lv_label_create(usb);
    lv_obj_set_style_text_font(usb_l, &lv_font_montserrat_24, 0);
    lv_label_set_text(usb_l, LV_SYMBOL_USB);
    lv_obj_center(usb_l);

    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, root_w, root_h - HEADER_H - FOOTER_H);
    lv_obj_set_y(s_list, HEADER_H);
    lv_obj_set_style_pad_hor(s_list, 12, 0);
    lv_obj_set_style_pad_bottom(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    /* The list ends above the strip where the swipe-up for the navigation bar starts */
    s_storage = lv_label_create(root);
    lv_obj_set_style_text_color(s_storage, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_storage, phone_font_by_size(16), 0);
    lv_obj_align(s_storage, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_label_set_text(s_storage, "");

    /* details sheet */
    s_sheet = lv_obj_create(root);
    lv_obj_remove_style_all(s_sheet);
    lv_obj_set_size(s_sheet, root_w, root_h);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_sheet, lv_color_black(), 0);
    lv_obj_add_flag(s_sheet, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sheet, on_sheet_close, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *card = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, root_w - 32, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);       /* a tap on the card does not close the sheet */
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    s_sheet_info = lv_label_create(card);
    lv_obj_set_width(s_sheet_info, LV_PCT(100));
    lv_label_set_long_mode(s_sheet_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(s_sheet_info, 6, 0);
    lv_label_set_text(s_sheet_info, "");
    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 12, 0);
    s_sheet_delete = make_button(btns, T("Xóa", "Delete"), on_delete);
    make_button(btns, T("Đóng", "Close"), on_sheet_close);

    /* text preview: name and close on top, the text scrolls above the navigation strip */
    s_text = lv_obj_create(root);
    lv_obj_remove_style_all(s_text);
    lv_obj_set_size(s_text, root_w, root_h);
    lv_obj_set_style_bg_opa(s_text, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_text, lv_color_hex(COLOR_BG), 0);
    lv_obj_add_flag(s_text, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(s_text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tc = lv_button_create(s_text);
    lv_obj_remove_style_all(tc);
    lv_obj_set_size(tc, 48, 44);
    lv_obj_set_style_radius(tc, 22, 0);
    lv_obj_set_style_bg_opa(tc, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tc, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_color(tc, lv_color_hex(0x3A444E), LV_STATE_PRESSED);
    lv_obj_align(tc, LV_ALIGN_TOP_LEFT, 12, 10);
    lv_obj_add_event_cb(tc, [](lv_event_t *) {
        close_text();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *tcl = lv_label_create(tc);
    lv_obj_set_style_text_font(tcl, &lv_font_montserrat_24, 0);
    lv_label_set_text(tcl, LV_SYMBOL_LEFT);
    lv_obj_center(tcl);
    s_text_title = lv_label_create(s_text);
    lv_label_set_long_mode(s_text_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_text_title, root_w - 88);
    lv_obj_set_height(s_text_title, lv_font_get_line_height(phone_font_by_size(20)));
    lv_obj_align(s_text_title, LV_ALIGN_TOP_LEFT, 72, 20);
    lv_obj_t *scroll = lv_obj_create(s_text);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_size(scroll, root_w, root_h - HEADER_H - NAV_STRIP_H);
    lv_obj_set_y(scroll, HEADER_H);
    lv_obj_set_style_pad_hor(scroll, 16, 0);
    lv_obj_set_style_pad_bottom(scroll, 12, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_ACTIVE);
    s_text_body = lv_label_create(scroll);
    lv_obj_set_width(s_text_body, LV_PCT(100));
    lv_label_set_long_mode(s_text_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_text_body, phone_font_by_size(16), 0);
    lv_obj_set_style_text_line_space(s_text_body, 4, 0);
    lv_label_set_text(s_text_body, "");

    /* why the USB drive could not start, over the list for a few seconds */
    s_usb_msg = lv_label_create(root);
    lv_obj_set_width(s_usb_msg, root_w - 48);
    lv_label_set_long_mode(s_usb_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_usb_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(s_usb_msg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_usb_msg, lv_color_hex(0x3A2326), 0);
    lv_obj_set_style_radius(s_usb_msg, 14, 0);
    lv_obj_set_style_pad_all(s_usb_msg, 14, 0);
    lv_obj_align(s_usb_msg, LV_ALIGN_TOP_MID, 0, HEADER_H + 8);
    lv_obj_add_flag(s_usb_msg, LV_OBJ_FLAG_HIDDEN);

    /* USB drive panel */
    s_usb = lv_obj_create(root);
    lv_obj_remove_style_all(s_usb);
    lv_obj_set_size(s_usb, root_w, root_h);
    lv_obj_set_style_bg_opa(s_usb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_usb, lv_color_hex(COLOR_BG), 0);
    lv_obj_add_flag(s_usb, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(s_usb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *big = lv_label_create(s_usb);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(big, lv_color_hex(COLOR_USB), 0);
    lv_label_set_text(big, LV_SYMBOL_USB);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_t *title = lv_label_create(s_usb);
    lv_obj_set_style_text_font(title, phone_font_by_size(24), 0);
    lv_label_set_text(title, T("Ổ USB", "USB drive"));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 230);
    s_usb_state = lv_label_create(s_usb);
    lv_obj_set_style_text_color(s_usb_state, lv_color_hex(COLOR_SUB), 0);
    lv_label_set_text(s_usb_state, "");
    lv_obj_align(s_usb_state, LV_ALIGN_TOP_MID, 0, 280);
    lv_obj_t *hint = lv_label_create(s_usb);
    lv_obj_set_width(hint, root_w - 64);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, phone_font_by_size(16), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_SUB), 0);
    lv_label_set_text(hint, T("Cắm cáp vào cổng USB HS (Type-C). Máy tính thấy thẻ nhớ như một ổ USB.\n\n"
                              "Đẩy ổ ra trên máy tính trước khi tắt. Thoát app cũng tắt ổ USB.",
                              "Plug the cable into the USB HS (Type-C) socket. The PC sees the card as a USB drive.\n\n"
                              "Eject the drive on the PC before turning it off. Leaving the app turns it off too."));
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 340);
    lv_obj_t *off = lv_button_create(s_usb);
    lv_obj_remove_style_all(off);
    lv_obj_set_size(off, root_w - 96, 56);
    lv_obj_set_style_radius(off, 16, 0);
    lv_obj_set_style_bg_opa(off, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(off, lv_color_hex(COLOR_USB), 0);
    lv_obj_set_style_bg_color(off, lv_color_hex(0x2452B8), LV_STATE_PRESSED);
    lv_obj_align(off, LV_ALIGN_BOTTOM_MID, 0, -96);
    lv_obj_add_event_cb(off, on_usb_off, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *offl = lv_label_create(off);
    lv_label_set_text(offl, T("Tắt ổ USB", "Turn off"));
    lv_obj_center(offl);
}

} // namespace

namespace esp_brookesia::apps {

PhoneFiles *PhoneFiles::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneFiles();
    }
    return _instance;
}

PhoneFiles::PhoneFiles():
    App(phone_tr("Tệp", "Files"), &phone_files_icon, true, true, true)
{
}

PhoneFiles::~PhoneFiles()
{
}

bool PhoneFiles::run()
{
    s_dir = ROOT;
    s_selected = -1;
    s_usb_msg_until = 0;
    build_ui();
    s_timer = lv_timer_create(on_timer, 250, nullptr);
    show_dir();
    return true;
}

/* Back closes the sheet, then goes up one folder, then leaves the app */
bool PhoneFiles::back()
{
    if (s_usb_on) {
        usb_stop();
        show_dir();
        return true;
    }
    if (text_open()) {
        close_text();
        return true;
    }
    if (s_selected >= 0) {
        close_sheet();
        return true;
    }
    if (s_dir.size() > strlen(ROOT)) {
        s_dir.erase(s_dir.rfind('/'));
        show_dir();
        return true;
    }
    return notifyCoreClosed();
}

/* Other apps may have added or removed files */
bool PhoneFiles::resume()
{
    if (s_root && s_selected < 0 && !text_open()) {
        show_dir();
    }
    return true;
}

/* Leaving the app gives the card back to the phone */
bool PhoneFiles::pause()
{
    usb_stop();
    return true;
}

bool PhoneFiles::close()
{
    usb_stop();
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneFiles, APP_NAME, []()
{
    return std::shared_ptr<PhoneFiles>(PhoneFiles::requestInstance(), [](PhoneFiles * p) {});
})

} // namespace esp_brookesia::apps
