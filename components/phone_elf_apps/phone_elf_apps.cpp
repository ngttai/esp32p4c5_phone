#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "phone_elf_apps.h"
#include "phone_elf_host.h"
#include "phone_locale.h"
#include "phone_paths.h"
#include "phone_storage.h"

#define USERAPPS_DIR PHONE_SD_ROOT "/userapps"

using namespace esp_brookesia::systems;

namespace {

const char *TAG = "ELFAPPS";

/* The launcher icon of a package: a rounded square in a color taken from the id (126 x 126 ARGB8888, like the built-in icons) */
lv_image_dsc_t *make_icon(const std::string &id)
{
    constexpr int W = 126;
    constexpr int R = 28;
    auto *dsc = static_cast<lv_image_dsc_t *>(calloc(1, sizeof(lv_image_dsc_t)));
    auto *px = static_cast<uint32_t *>(heap_caps_malloc(W * W * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!dsc || !px) {
        free(dsc);
        heap_caps_free(px);
        return nullptr;
    }
    uint32_t h = 2166136261u;
    for (char c : id) {
        h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    const uint8_t base_r = 60 + (h & 0x7F), base_g = 60 + ((h >> 8) & 0x7F), base_b = 90 + ((h >> 16) & 0x7F);
    for (int y = 0; y < W; y++) {
        for (int x = 0; x < W; x++) {
            /* distance outside the rounded corners */
            int dx = std::max(std::max(R - x, x - (W - 1 - R)), 0);
            int dy = std::max(std::max(R - y, y - (W - 1 - R)), 0);
            bool inside = dx * dx + dy * dy <= R * R;
            /* a lighter ring at the middle, a diagonal shade */
            int cx = x - W / 2, cy = y - W / 2;
            int d2 = cx * cx + cy * cy;
            bool ring = d2 < 34 * 34 && d2 > 26 * 26;
            int shade = 100 - (x + y) / 6;
            uint32_t r = std::min(255, base_r * shade / 100 + (ring ? 90 : 0));
            uint32_t g = std::min(255, base_g * shade / 100 + (ring ? 90 : 0));
            uint32_t b = std::min(255, base_b * shade / 100 + (ring ? 90 : 0));
            px[y * W + x] = inside ? (0xFF000000u | (r << 16) | (g << 8) | b) : 0u;
        }
    }
    dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.w = W;
    dsc->header.h = W;
    dsc->data_size = W * W * 4;
    dsc->data = reinterpret_cast<const uint8_t *>(px);
    return dsc;
}

/* One installed ELF package. Lives as long as the firmware runs (a launcher entry), unless it is uninstalled. */
class ElfApp: public phone::App {
public:
    ElfApp(const char *name, const lv_image_dsc_t *icon, std::string id, std::string elf_path):
        App(name, icon, true, true, phone_nav_bar()), id_(std::move(id)), elf_path_(std::move(elf_path))
    {
    }

protected:
    bool run() override
    {
        char err[64] = {};
        int r = phone_elf_host_open(id_.c_str(), elf_path_.c_str(), lv_screen_active(), &ElfApp::on_close_request, this, err, sizeof(err));
        if (r != 0) {
            ESP_LOGW(TAG, "[%s] cannot open: %s", id_.c_str(), err);
            lv_obj_t *l = lv_label_create(lv_screen_active());
            lv_label_set_text_fmt(l, "%s\n%s", id_.c_str(), err);
            lv_obj_set_style_text_color(l, lv_color_hex(0xFF6060), 0);
            lv_obj_center(l);
            owns_host_ = false;
        } else {
            owns_host_ = true;
        }
        return true;
    }
    bool back() override
    {
        return notifyCoreClosed();
    }
    bool close() override
    {
        if (owns_host_) {
            phone_elf_host_close();
            owns_host_ = false;
        }
        return true;
    }

private:
    static void on_close_request(void *user)
    {
        static_cast<ElfApp *>(user)->notifyCoreClosed();
    }

    std::string id_;
    std::string elf_path_;
    bool owns_host_ = false;
};

struct Package {
    std::string id;
    std::string name;
    std::string elf_path;
};

std::string read_text(const std::string &path)
{
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return out;
    }
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < 16384) {
        out.append(buf, n);
    }
    fclose(f);
    return out;
}

bool safe_relative(const char *p)
{
    return p && p[0] && p[0] != '/' && !strstr(p, "..");
}

bool safe_id(const char *s)
{
    if (!s || !s[0] || strlen(s) > 32) {
        return false;
    }
    for (const char *c = s; *c; c++) {
        if (!isalnum(static_cast<unsigned char>(*c)) && *c != '_' && *c != '-' && *c != '.') {
            return false;
        }
    }
    return true;
}

bool parse_package(const std::string &folder, Package &out)
{
    const std::string dir = std::string(USERAPPS_DIR) + "/" + folder;
    const std::string text = read_text(dir + "/manifest.json");
    cJSON *root = cJSON_Parse(text.c_str());
    if (!root) {
        ESP_LOGW(TAG, "%s: manifest.json missing or not JSON", folder.c_str());
        return false;
    }
    bool ok = false;
    cJSON *pkg = cJSON_GetObjectItem(root, "package");
    cJSON *rt = cJSON_GetObjectItem(root, "runtime");
    cJSON *id = pkg ? cJSON_GetObjectItem(pkg, "id") : nullptr;
    cJSON *type = rt ? cJSON_GetObjectItem(rt, "type") : nullptr;
    cJSON *entry = rt ? cJSON_GetObjectItem(rt, "entry") : nullptr;
    if (!cJSON_IsString(id) || !cJSON_IsString(type) || !cJSON_IsString(entry)) {
        ESP_LOGW(TAG, "%s: package.id, runtime.type and runtime.entry are required", folder.c_str());
    } else if (strcasecmp(type->valuestring, "ELF") != 0) {
        ESP_LOGI(TAG, "%s: runtime %s is not supported here, skipped", folder.c_str(), type->valuestring);
    } else if (!safe_id(id->valuestring) || !safe_relative(entry->valuestring)) {
        ESP_LOGW(TAG, "%s: unsafe id or entry path", folder.c_str());
    } else {
        out.id = id->valuestring;
        out.elf_path = dir + "/" + entry->valuestring;
        out.name = out.id;
        cJSON *name = cJSON_GetObjectItem(pkg, "name");
        if (cJSON_IsString(name)) {
            out.name = name->valuestring;
        } else if (cJSON_IsObject(name)) {
            cJSON *vi = cJSON_GetObjectItem(name, "vi");
            cJSON *en = cJSON_GetObjectItem(name, "en");
            const char *v = cJSON_IsString(vi) ? vi->valuestring : nullptr;
            const char *e = cJSON_IsString(en) ? en->valuestring : nullptr;
            const char *pick = (v && e) ? phone_tr(v, e) : (e ? e : v);
            if (pick && pick[0]) {
                out.name = pick;
            }
        }
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

void scan_task(void *arg)
{
    auto *phone_sys = static_cast<phone::Phone *>(arg);

    /* wait for the card (mounted in the background after the start) */
    for (int i = 0; i < 120 && phone_storage_get(nullptr, nullptr) != PHONE_STORAGE_MOUNTED; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (phone_storage_get(nullptr, nullptr) != PHONE_STORAGE_MOUNTED) {
        ESP_LOGW(TAG, "no card, no apps from the card");
        vTaskDelete(nullptr);
    }

    std::vector<Package> packages;
    if (DIR *d = opendir(USERAPPS_DIR)) {
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') {
                continue;
            }
            Package p;
            if (parse_package(e->d_name, p)) {
                bool dup = std::any_of(packages.begin(), packages.end(), [&](const Package & q) {
                    return q.id == p.id;
                });
                if (dup) {
                    ESP_LOGW(TAG, "%s: id %s already used, skipped", e->d_name, p.id.c_str());
                } else {
                    packages.push_back(std::move(p));
                }
            }
        }
        closedir(d);
    } else {
        ESP_LOGI(TAG, "no %s on the card", USERAPPS_DIR);
    }
    std::sort(packages.begin(), packages.end(), [](const Package & a, const Package & b) {
        return a.id < b.id;
    });

    for (auto &p : packages) {
        lv_image_dsc_t *icon = make_icon(p.id);
        char *name = strdup(p.name.c_str());       /* the app keeps the pointer: kept for good */
        if (!icon || !name || !bsp_display_lock(5000)) {
            ESP_LOGW(TAG, "%s: out of memory or no display lock", p.id.c_str());
            continue;
        }
        auto *app = new ElfApp(name, icon, p.id, p.elf_path);
        int id = phone_sys->installApp(app);
        bsp_display_unlock();
        ESP_LOGI(TAG, "installed '%s' (%s) as app %d", name, p.id.c_str(), id);
        if (id < 0) {
            ESP_LOGW(TAG, "install failed for %s", p.id.c_str());
        }
    }
    ESP_LOGI(TAG, "%u app(s) from the card", (unsigned)packages.size());
    vTaskDelete(nullptr);
}

} // namespace

void phone_elf_apps_start(phone::Phone *phone_sys)
{
    xTaskCreate(scan_task, "elfapps", 6144, phone_sys, 3, nullptr);
}
