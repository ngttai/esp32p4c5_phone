/*
 * Apps from the memory card.
 *   /sdcard/packages/<name>.bpk  packages waiting to be installed: a ZIP with manifest.json at the root (fields of an ESP-Brookesia 0.8
 *                                package) and the ELF it names; the Apps app unpacks one to userapps/<id>/ and installs it
 *   /sdcard/userapps/<id>/       installed packages: every one is an app on the launcher (loaded at the start, or installed by the Apps app)
 */
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "phone_elf_apps.h"
#include "phone_elf_host.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_paths.h"
#include "phone_zip.h"
#include "phone_storage.h"

#define USERAPPS_DIR PHONE_SD_ROOT "/userapps"
#define PACKAGES_DIR PHONE_SD_ROOT "/packages"

extern "C" const lv_image_dsc_t phone_apps_icon;

using namespace esp_brookesia::systems;

namespace {

const char *TAG = "ELFAPPS";

/* The launcher icon of a package, drawn when needed (126 x 126 ARGB8888 in PSRAM, like the built-in icons): a gradient tile with
 * a soft shine, in a hue taken from the id, and a white glyph: a chip for a package, a 2 x 2 grid for the Apps app. */
struct Rgb {
    float r, g, b;
};

Rgb from_hsv(float h, float s, float v)
{
    h = h - floorf(h);
    float k = h * 6.0f;
    int i = static_cast<int>(k);
    float f = k - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}

/* signed distance to a rounded rectangle (negative inside) */
float sd_round_rect(float x, float y, float cx, float cy, float hw, float hh, float r)
{
    float dx = fabsf(x - cx) - (hw - r), dy = fabsf(y - cy) - (hh - r);
    float ox = fmaxf(dx, 0.0f), oy = fmaxf(dy, 0.0f);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(dx, dy), 0.0f) - r;
}

float cover(float sd)      // coverage of a shape from its distance, one pixel of anti-aliasing
{
    return fminf(fmaxf(0.5f - sd, 0.0f), 1.0f);
}

lv_image_dsc_t *make_icon(const std::string &id, bool grid = false)
{
    constexpr int W = 126;
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
    const float hue = grid ? 0.58f : (h % 360) / 360.0f;
    const Rgb top = from_hsv(hue, 0.55f, 1.00f), bot = from_hsv(hue + 0.06f, 0.85f, 0.62f);

    for (int y = 0; y < W; y++) {
        for (int x = 0; x < W; x++) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float c = W / 2.0f;
            const float a_tile = cover(sd_round_rect(fx, fy, c, c, W / 2.0f, W / 2.0f, 30.0f));
            if (a_tile <= 0.0f) {
                px[y * W + x] = 0;
                continue;
            }
            /* diagonal gradient + a shine on the upper part */
            float t = (fy * 0.8f + fx * 0.2f) / W;
            Rgb col = {top.r + (bot.r - top.r) * t, top.g + (bot.g - top.g) * t, top.b + (bot.b - top.b) * t};
            float shine = fmaxf(0.0f, 1.0f - fy / (W * 0.55f)) * 0.16f;
            col = {col.r + (1 - col.r) * shine, col.g + (1 - col.g) * shine, col.b + (1 - col.b) * shine};

            /* glyph: white shapes over the tile, a faint shadow under them */
            float glyph = 0.0f, shadow = 0.0f;
            if (grid) {
                const float tile = 22.0f, gap = 8.0f, o = tile + gap / 2.0f;
                for (int gy = 0; gy < 2; gy++) {
                    for (int gx = 0; gx < 2; gx++) {
                        float cx = c + (gx ? o : -o) * 1.0f, cy = c + (gy ? o : -o) * 1.0f;
                        glyph = fmaxf(glyph, cover(sd_round_rect(fx, fy, cx, cy, tile - 1, tile - 1, 9.0f)));
                        shadow = fmaxf(shadow, cover(sd_round_rect(fx, fy + 3, cx, cy, tile - 1, tile - 1, 9.0f)));
                    }
                }
            } else {
                /* chip: a body, a darker die, and four pins on every side */
                float body = cover(sd_round_rect(fx, fy, c, c, 29, 29, 9.0f));
                float body_sh = cover(sd_round_rect(fx, fy + 3, c, c, 29, 29, 9.0f));
                float die = cover(sd_round_rect(fx, fy, c, c, 15, 15, 5.0f));
                float pins = 0.0f;
                for (int i = 0; i < 4; i++) {
                    float off = -19.5f + i * 13.0f;
                    pins = fmaxf(pins, cover(sd_round_rect(fx, fy, c + off, c - 40, 3.5f, 8, 2.0f)));
                    pins = fmaxf(pins, cover(sd_round_rect(fx, fy, c + off, c + 40, 3.5f, 8, 2.0f)));
                    pins = fmaxf(pins, cover(sd_round_rect(fx, fy, c - 40, c + off, 8, 3.5f, 2.0f)));
                    pins = fmaxf(pins, cover(sd_round_rect(fx, fy, c + 40, c + off, 8, 3.5f, 2.0f)));
                }
                glyph = fmaxf(pins, body * (1.0f - die * 0.85f));
                shadow = fmaxf(body_sh, pins * 0.6f);
            }
            col = {col.r * (1 - 0.28f * shadow), col.g * (1 - 0.28f * shadow), col.b * (1 - 0.28f * shadow)};
            const float g = glyph * 0.96f;
            col = {col.r + (1 - col.r) * g, col.g + (1 - col.g) * g, col.b + (1 - col.b) * g};

            auto to8 = [](float v) {
                return static_cast<uint32_t>(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f);
            };
            px[y * W + x] = (to8(a_tile) << 24) | (to8(col.r) << 16) | (to8(col.g) << 8) | to8(col.b);
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

/* A PNG (any size, with or without alpha) as a 126 x 126 ARGB8888 image, like the built-in icons. nullptr when it cannot be decoded. */
lv_image_dsc_t *decode_icon(const uint8_t *data, size_t len)
{
    constexpr int W = 126;
    lv_image_dsc_t raw = {};
    raw.header.cf = LV_COLOR_FORMAT_RAW;
    raw.header.magic = LV_IMAGE_HEADER_MAGIC;
    raw.data = data;
    raw.data_size = len;
    lv_image_decoder_dsc_t d = {};
    if (lv_image_decoder_open(&d, &raw, nullptr) != LV_RESULT_OK || !d.decoded) {
        return nullptr;
    }
    const lv_draw_buf_t *src = d.decoded;
    const int sw = src->header.w, sh = src->header.h;
    lv_image_dsc_t *dsc = nullptr;
    uint32_t *px = nullptr;
    if (src->header.cf == LV_COLOR_FORMAT_ARGB8888 && sw > 0 && sh > 0) {
        dsc = static_cast<lv_image_dsc_t *>(calloc(1, sizeof(lv_image_dsc_t)));
        px = static_cast<uint32_t *>(heap_caps_malloc(W * W * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (dsc && px) {
        for (int y = 0; y < W; y++) {       // nearest neighbour, any source size
            const uint32_t *row = reinterpret_cast<const uint32_t *>(src->data + static_cast<size_t>(y * sh / W) * src->header.stride);
            for (int x = 0; x < W; x++) {
                px[y * W + x] = row[x * sw / W];
            }
        }
        dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
        dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc->header.w = W;
        dsc->header.h = W;
        dsc->data_size = W * W * 4;
        dsc->data = reinterpret_cast<const uint8_t *>(px);
    } else {
        free(dsc);
        heap_caps_free(px);
        dsc = nullptr;
    }
    lv_image_decoder_close(&d);
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
        return open_host();
    }
    bool resume() override
    {
        // back from the recents after another ELF app took the host: start again
        return owns_host_ ? true : open_host();
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
            if (s_owner == this) {
                s_owner = nullptr;
            }
        }
        return true;
    }

private:
    static void on_close_request(void *user)
    {
        static_cast<ElfApp *>(user)->notifyCoreClosed();
    }

    /* One ELF runs at a time. An ELF app that is open in the background gives the host up to the one that opens now. */
    bool open_host()
    {
        if (phone_elf_host_is_open() && s_owner && s_owner != this) {
            ESP_LOGI(TAG, "[%s] takes the host from [%s]", id_.c_str(), s_owner->id_.c_str());
            phone_elf_host_close();
            ElfApp *old = s_owner;
            old->owns_host_ = false;
            s_owner = nullptr;
            old->notifyCoreClosed();    // the launcher closes it for good (recents, status bar); its close() finds nothing to unload
        }
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
            s_owner = this;
        }
        return true;
    }

    inline static ElfApp *s_owner = nullptr;
    std::string id_;
    std::string elf_path_;
    bool owns_host_ = false;
};


struct Package {
    std::string folder;     // folder name under userapps/ or packages/
    std::string id;
    std::string name;
    std::string version;
    std::string elf_path;   // full path (below the folder it was found in)
    std::string dir;        // folder of the package (empty inside a .bpk)
    std::string icon_file;  // package.icon, "icon.png" when the manifest does not say
    lv_image_dsc_t *icon = nullptr;   // its icon when it could be read (a package waiting in packages/)
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

/* A whole file in a malloc'ed buffer (free() it), nullptr when it is missing or larger than `max`. */
uint8_t *read_bin(const std::string &path, size_t max, size_t *len)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (n > 0 && static_cast<size_t>(n) <= max) ? static_cast<uint8_t *>(malloc(n)) : nullptr;
    if (buf && fread(buf, 1, n, f) != static_cast<size_t>(n)) {
        free(buf);
        buf = nullptr;
    }
    fclose(f);
    *len = n;
    return buf;
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

/* Parse the text of a manifest.json; `dir` is the folder the entry path is relative to. */
bool parse_manifest(const std::string &text, const std::string &folder, const std::string &dir, Package &out, bool quiet)
{
    cJSON *root = cJSON_Parse(text.c_str());
    if (!root) {
        if (!quiet) {
            ESP_LOGW(TAG, "%s: manifest.json missing or not JSON", folder.c_str());
        }
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
        out.folder = folder;
        out.dir = dir;
        out.id = id->valuestring;
        out.elf_path = dir + "/" + entry->valuestring;
        out.name = out.id;
        cJSON *icon = cJSON_GetObjectItem(pkg, "icon");
        out.icon_file = (cJSON_IsString(icon) && safe_relative(icon->valuestring)) ? icon->valuestring : "icon.png";
        cJSON *ver = cJSON_GetObjectItem(pkg, "version");
        out.version = cJSON_IsString(ver) ? ver->valuestring : "";
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

/* manifest.json of <base>/<folder> */
bool parse_package(const std::string &base, const std::string &folder, Package &out, bool quiet = false)
{
    const std::string dir = base + "/" + folder;
    return parse_manifest(read_text(dir + "/manifest.json"), folder, dir, out, quiet);
}

std::vector<Package> scan_dir(const char *base)
{
    std::vector<Package> out;
    if (DIR *d = opendir(base)) {
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') {
                continue;
            }
            Package p;
            if (parse_package(base, e->d_name, p, true)) {
                out.push_back(std::move(p));
            }
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const Package & a, const Package & b) {
        return a.id < b.id;
    });
    return out;
}

/* ---- files ---- */
void remove_dir(const std::string &path, int depth = 0)
{
    if (DIR *d = opendir(path.c_str())) {
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') {
                continue;
            }
            const std::string p = path + "/" + e->d_name;
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && depth < 4) {
                remove_dir(p, depth + 1);
            } else {
                remove(p.c_str());
            }
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

/* ---- the installed packages ---- */
struct Installed {
    ElfApp *app;
    int app_id;
    std::string folder;
};
struct Meta {                       // kept for good: the launcher keeps the pointers of an app it showed
    lv_image_dsc_t *icon;
    char *name;
    bool from_package;
};

phone::Phone *s_phone;
std::map<std::string, Installed> s_installed;   // by package id
std::map<std::string, Meta> s_meta;
std::map<std::string, lv_image_dsc_t *> s_repo_icons;   // icons of the packages in packages/, by file name + size + time

/* Install the package of userapps/<folder> (the LVGL lock is held by the caller). Empty string = done, else the reason. */
std::string install_folder(const std::string &folder)
{
    Package p;
    if (!parse_package(USERAPPS_DIR, folder, p)) {
        return "manifest.json";
    }
    if (s_installed.count(p.id)) {
        return "already installed";
    }
    auto &meta = s_meta[p.id];
    if (!meta.icon || !meta.from_package) {        // the icon of the package, else the drawn one
        size_t plen = 0;
        if (uint8_t *png = read_bin(p.dir + "/" + p.icon_file, 256 * 1024, &plen)) {
            if (lv_image_dsc_t *icon = decode_icon(png, plen)) {
                meta.icon = icon;
                meta.from_package = true;
            }
            free(png);
        }
        if (!meta.icon) {
            meta.icon = make_icon(p.id);
        }
    }
    if (!meta.name || p.name != meta.name) {
        meta.name = strdup(p.name.c_str());
    }
    if (!meta.icon || !meta.name) {
        return "out of memory";
    }
    auto *app = new ElfApp(meta.name, meta.icon, p.id, p.elf_path);
    int id = s_phone->installApp(app);
    if (id < 0) {
        delete app;
        return "the launcher refused it";
    }
    s_installed[p.id] = Installed{app, id, folder};
    ESP_LOGI(TAG, "installed '%s' (%s) as app %d", meta.name, p.id.c_str(), id);
    return "";
}

bool is_running(int app_id)
{
    return s_phone->getManager().getRunningAppById(app_id) != nullptr;
}

/* Remove the app and its folder. Empty string = done, else the reason. */
std::string uninstall_package(const std::string &pkg_id)
{
    auto it = s_installed.find(pkg_id);
    if (it == s_installed.end()) {
        return "not installed";
    }
    if (is_running(it->second.app_id)) {
        return "close the app first";
    }
    ElfApp *app = it->second.app;
    const std::string folder = it->second.folder;
    if (!s_phone->uninstallApp(app)) {
        return "the launcher refused it";
    }
    delete app;
    s_installed.erase(it);
    remove_dir(std::string(USERAPPS_DIR) + "/" + folder);
    ESP_LOGI(TAG, "uninstalled %s", pkg_id.c_str());
    return "";
}

/* The packages waiting in packages/: every *.bpk whose manifest.json can be read ("folder" holds the file name). */
std::vector<Package> scan_bpk()
{
    std::vector<Package> out;
    if (DIR *d = opendir(PACKAGES_DIR)) {
        while (dirent *e = readdir(d)) {
            const size_t n = strlen(e->d_name);
            if (e->d_name[0] == '.' || n < 5 || strcasecmp(e->d_name + n - 4, ".bpk") != 0) {
                continue;
            }
            char *text = nullptr;
            size_t len = 0;
            char err[48];
            const std::string path = std::string(PACKAGES_DIR) + "/" + e->d_name;
            if (phone_zip_read_file(path.c_str(), "manifest.json", &text, &len, err, sizeof(err)) != 0) {
                ESP_LOGW(TAG, "%s: %s", e->d_name, err);
                continue;
            }
            Package p;
            if (parse_manifest(std::string(text, len), e->d_name, "", p, false)) {
                struct stat st;
                const std::string key = std::string(e->d_name) + "|" + std::to_string(stat(path.c_str(), &st) == 0 ? st.st_size : 0) +
                                        "|" + std::to_string(st.st_mtime);
                auto hit = s_repo_icons.find(key);
                if (hit == s_repo_icons.end()) {       // read and decoded once per file (63 KB of PSRAM each)
                    lv_image_dsc_t *icon = nullptr;
                    char *png = nullptr;
                    size_t plen = 0;
                    if (phone_zip_read_file(path.c_str(), p.icon_file.c_str(), &png, &plen, err, sizeof(err)) == 0) {
                        icon = decode_icon(reinterpret_cast<const uint8_t *>(png), plen);
                        free(png);
                    }
                    hit = s_repo_icons.emplace(key, icon).first;
                }
                p.icon = hit->second;
                out.push_back(std::move(p));
            }
            free(text);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const Package & a, const Package & b) {
        return a.id < b.id;
    });
    return out;
}

/* Unpack packages/<file>.bpk into userapps/<id>/ and install it. Empty string = done, else the reason. */
std::string install_from_bpk(const std::string &file)
{
    const std::string path = std::string(PACKAGES_DIR) + "/" + file;
    char *text = nullptr;
    size_t len = 0;
    char err[64];
    if (phone_zip_read_file(path.c_str(), "manifest.json", &text, &len, err, sizeof(err)) != 0) {
        return err;
    }
    Package p;
    bool ok = parse_manifest(std::string(text, len), file, "", p, false);
    free(text);
    if (!ok) {
        return "manifest.json";
    }
    if (s_installed.count(p.id)) {
        return "already installed";
    }
    mkdir(USERAPPS_DIR, 0775);
    const std::string dst = std::string(USERAPPS_DIR) + "/" + p.id;
    const std::string tmp = dst + ".tmp";
    struct stat st;
    if (stat(dst.c_str(), &st) == 0) {
        return "a folder with this id is already in userapps";
    }
    remove_dir(tmp);
    if (phone_zip_extract(path.c_str(), tmp.c_str(), err, sizeof(err)) != 0) {
        remove_dir(tmp);
        return err;
    }
    if (rename(tmp.c_str(), dst.c_str()) != 0) {
        remove_dir(tmp);
        return "cannot move the folder";
    }
    std::string e2 = install_folder(p.id);
    if (!e2.empty()) {
        remove_dir(dst);
    }
    return e2;
}

/* ---- the Apps app ---- */
constexpr uint32_t C_BG = 0x101418, C_CARD = 0x1E252C, C_TEXT = 0xFFFFFF, C_SUB = 0x9AA5B1, C_OK = 0x2F80ED, C_WARN = 0xF87171, C_OK2 = 0x34D399;

class ElfAppManager: public phone::App {
public:
    ElfAppManager(const char *name, const lv_image_dsc_t *icon): App(name, icon, true, true, phone_nav_bar()) {}

protected:
    bool run() override;
    bool back() override
    {
        return notifyCoreClosed();
    }
    bool close() override
    {
        // the screen goes with the app: nothing may touch its objects after this (the "sure?" timer would)
        if (s_confirm_timer) {
            lv_timer_delete(s_confirm_timer);
            s_confirm_timer = nullptr;
        }
        s_confirm = -1;
        s_root = nullptr;
        s_list = nullptr;
        s_status_label = nullptr;
        s_rows.clear();
        return true;
    }

private:
    static void build();
    static void rebuild();
    static void on_action(lv_event_t *e);
    static void on_refresh(lv_event_t *)
    {
        s_confirm = -1;
        s_status.clear();
        rebuild();
    }

    struct Row {
        std::string id;
        std::string folder;
        bool installed;
    };
    inline static lv_obj_t *s_root = nullptr;
    inline static lv_obj_t *s_list = nullptr;
    inline static lv_obj_t *s_status_label = nullptr;
    inline static std::vector<Row> s_rows;
    inline static std::string s_status;
    inline static int s_confirm = -1;           // row waiting for the second tap of "remove"
    inline static lv_timer_t *s_confirm_timer = nullptr;
    friend void make_manager_row(const Package &p, bool installed, int idx);
};

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    return o;
}

lv_obj_t *text(lv_obj_t *parent, const char *t, int size, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, phone_font_by_size(size), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

void ElfAppManager::build()
{
    lv_obj_t *root = plain(lv_screen_active());
    s_root = root;
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_hor(root, 16, 0);
    lv_obj_set_style_pad_top(root, 14, 0);
    lv_obj_set_style_pad_bottom(root, 40, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *head = plain(root);
    lv_obj_set_size(head, LV_PCT(100), 52);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(head, phone_tr("Ứng dụng từ thẻ", "Apps from the card"), 24, C_TEXT);
    lv_obj_t *btn = lv_button_create(head);
    lv_obj_set_size(btn, 120, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_CARD), 0);
    lv_obj_add_event_cb(btn, on_refresh, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = text(btn, phone_tr("Làm mới", "Refresh"), 18, C_TEXT);
    lv_obj_center(bl);

    s_status_label = text(root, "", 16, C_SUB);
    lv_obj_set_width(s_status_label, LV_PCT(100));

    s_list = plain(root);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    rebuild();
}

void make_manager_row(const Package &p, bool installed, int idx)
{
    lv_obj_t *card = plain(ElfAppManager::s_list);
    lv_obj_set_size(card, LV_PCT(100), 84);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_image_dsc_t *icon = p.icon;
    if (installed) {
        auto m = s_meta.find(p.id);
        icon = m != s_meta.end() ? m->second.icon : nullptr;
    }
    if (!icon) {
        auto &m = s_meta[p.id];                 // a package without a readable icon: the drawn one
        if (!m.icon) {
            m.icon = make_icon(p.id);
        }
        icon = m.icon;
    }
    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, icon);
    lv_obj_set_size(img, 56, 56);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_margin_right(img, 12, 0);
    lv_obj_t *col = plain(card);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    text(col, p.name.c_str(), 20, C_TEXT);
    std::string sub = p.id + (p.version.empty() ? "" : "  v" + p.version);
    text(col, sub.c_str(), 15, C_SUB);

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, 110, 46);
    const bool confirming = installed && ElfAppManager::s_confirm == idx;
    lv_obj_set_style_bg_color(btn, lv_color_hex(installed ? (confirming ? C_WARN : 0x3A4450) : C_OK), 0);
    lv_obj_add_event_cb(btn, ElfAppManager::on_action, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(idx)));
    const char *label = installed ? (confirming ? phone_tr("Chắc chắn?", "Sure?") : phone_tr("Gỡ", "Remove")) : phone_tr("Cài", "Install");
    lv_obj_t *l = text(btn, label, 18, C_TEXT);
    lv_obj_center(l);
}

void ElfAppManager::rebuild()
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);
    s_rows.clear();

    auto installed = scan_dir(USERAPPS_DIR);
    auto repo = scan_bpk();
    int idx = 0;
    bool any = false;
    lv_obj_t *h = text(s_list, phone_tr("Đã cài", "Installed"), 18, C_OK2);
    (void)h;
    for (auto &p : installed) {
        if (!s_installed.count(p.id)) {
            continue;       // in userapps/ but not loaded (wrong manifest, or added after the start: Install does not apply)
        }
        s_rows.push_back({p.id, p.folder, true});
        make_manager_row(p, true, idx++);
        any = true;
    }
    if (!any) {
        text(s_list, phone_tr("(chưa có)", "(none)"), 16, C_SUB);
    }
    text(s_list, phone_tr("Trong kho (/packages/*.bpk)", "Available (/packages/*.bpk)"), 18, C_OK);
    bool any_repo = false;
    for (auto &p : repo) {
        if (s_installed.count(p.id)) {
            continue;
        }
        s_rows.push_back({p.id, p.folder, false});
        make_manager_row(p, false, idx++);
        any_repo = true;
    }
    if (!any_repo) {
        text(s_list, phone_tr("(trống: chép file .bpk vào /packages trên thẻ)", "(empty: copy a .bpk file to /packages on the card)"), 16, C_SUB);
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, s_status.c_str());
    }
}

void ElfAppManager::on_action(lv_event_t *e)
{
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(s_rows.size())) {
        return;
    }
    const Row row = s_rows[idx];
    if (row.installed) {
        if (s_confirm != idx) {         // first tap: ask again within 3 s
            s_confirm = idx;
            if (s_confirm_timer) {
                lv_timer_delete(s_confirm_timer);
            }
            s_confirm_timer = lv_timer_create([](lv_timer_t *t) {
                s_confirm = -1;
                s_confirm_timer = nullptr;
                lv_timer_delete(t);
                if (s_list) {
                    rebuild();
                }
            }, 3000, nullptr);
            rebuild();
            return;
        }
        s_confirm = -1;
        if (s_confirm_timer) {
            lv_timer_delete(s_confirm_timer);
            s_confirm_timer = nullptr;
        }
        std::string err = uninstall_package(row.id);
        s_status = err.empty() ? std::string(phone_tr("Đã gỡ ", "Removed ")) + row.id : std::string(phone_tr("Không gỡ được: ", "Cannot remove: ")) + err;
    } else {
        std::string err = install_from_bpk(row.folder);
        s_status = err.empty() ? std::string(phone_tr("Đã cài ", "Installed ")) + row.id : std::string(phone_tr("Không cài được: ", "Cannot install: ")) + err;
    }
    rebuild();
}

bool ElfAppManager::run()
{
    s_confirm = -1;
    s_status.clear();
    build();
    return true;
}

void scan_task(void *arg)
{
    /* wait for the card (mounted in the background after the start) */
    for (int i = 0; i < 120 && phone_storage_get(nullptr, nullptr) != PHONE_STORAGE_MOUNTED; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (phone_storage_get(nullptr, nullptr) != PHONE_STORAGE_MOUNTED) {
        ESP_LOGW(TAG, "no card, no apps from the card");
        vTaskDelete(nullptr);
    }
    mkdir(USERAPPS_DIR, 0775);
    mkdir(PACKAGES_DIR, 0775);

    auto packages = scan_dir(USERAPPS_DIR);
    std::vector<std::string> seen;
    for (auto &p : packages) {
        if (std::find(seen.begin(), seen.end(), p.id) != seen.end()) {
            ESP_LOGW(TAG, "%s: id %s already used, skipped", p.folder.c_str(), p.id.c_str());
            continue;
        }
        seen.push_back(p.id);
        if (!bsp_display_lock(5000)) {
            continue;
        }
        std::string err = install_folder(p.folder);
        bsp_display_unlock();
        if (!err.empty()) {
            ESP_LOGW(TAG, "%s: %s", p.folder.c_str(), err.c_str());
        }
    }
    ESP_LOGI(TAG, "%u app(s) from the card", (unsigned)s_installed.size());
    vTaskDelete(nullptr);
}

} // namespace

void phone_elf_apps_start(phone::Phone *phone_sys)
{
    s_phone = phone_sys;
    xTaskCreate(scan_task, "elfapps", 8192, nullptr, 3, nullptr);
}

namespace esp_brookesia::apps {
/* The launcher entry of the manager, registered like the built-in apps */
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, ElfAppManager, "ElfAppManager", []()
{
    return std::shared_ptr<ElfAppManager>(new ElfAppManager(phone_tr("Ứng dụng", "Apps"), &phone_apps_icon));
})
}
