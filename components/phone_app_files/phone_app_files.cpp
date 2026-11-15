/*
 * Files app: browse the microSD card. A tap opens a folder or shows the details of a file;
 * a long press does the same for a folder. Files and empty folders can be deleted.
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
std::string s_dir;
std::vector<Entry> s_entries;
int s_selected = -1;
uint32_t s_delete_armed_until;

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
    if (s_root && s_delete_armed_until && lv_tick_get() >= s_delete_armed_until) {
        disarm_delete();
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
    lv_obj_set_width(s_path, root_w - 88);
    lv_obj_set_height(s_path, lv_font_get_line_height(phone_font_by_size(20)));
    lv_obj_align(s_path, LV_ALIGN_TOP_LEFT, 72, 20);
    lv_label_set_text(s_path, "/");

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
    build_ui();
    lv_timer_create(on_timer, 250, nullptr);
    show_dir();
    return true;
}

/* Back closes the sheet, then goes up one folder, then leaves the app */
bool PhoneFiles::back()
{
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
    if (s_root && s_selected < 0) {
        show_dir();
    }
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneFiles, APP_NAME, []()
{
    return std::shared_ptr<PhoneFiles>(PhoneFiles::requestInstance(), [](PhoneFiles * p) {});
})

} // namespace esp_brookesia::apps
