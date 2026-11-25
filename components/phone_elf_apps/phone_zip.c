#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "zlib.h"
#include "phone_zip.h"

#define MAX_ENTRIES     256
#define MAX_ENTRY_SIZE  (4u * 1024 * 1024)
#define MAX_TOTAL_SIZE  (16u * 1024 * 1024)
#define MAX_CD_SIZE     (128u * 1024)

typedef struct {
    char name[160];
    uint16_t method;
    uint32_t crc, csize, usize, local_off;
} entry_t;

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n) {
        snprintf(err, n, "%s", msg);
    }
}

static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Read the central directory: entries (malloc'ed array) and their count. */
static int read_directory(FILE *f, entry_t **out, int *count, char *err, size_t err_len)
{
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 22) {
        set_err(err, err_len, "not a ZIP file");
        return -1;
    }
    long tail = size < 66000 ? size : 66000;
    uint8_t *buf = malloc(tail);
    if (!buf) {
        set_err(err, err_len, "out of memory");
        return -1;
    }
    fseek(f, size - tail, SEEK_SET);
    if (fread(buf, 1, tail, f) != (size_t)tail) {
        free(buf);
        set_err(err, err_len, "read error");
        return -1;
    }
    long e = tail - 22;
    while (e >= 0 && u32(buf + e) != 0x06054b50u) {
        e--;
    }
    if (e < 0) {
        free(buf);
        set_err(err, err_len, "not a ZIP file (no end record)");
        return -1;
    }
    int n = u16(buf + e + 10);
    uint32_t cd_size = u32(buf + e + 12), cd_off = u32(buf + e + 16);
    free(buf);
    if (n <= 0 || n > MAX_ENTRIES || cd_size > MAX_CD_SIZE || cd_off + cd_size > (uint32_t)size) {
        set_err(err, err_len, "unsupported ZIP (entries or size)");
        return -1;
    }
    uint8_t *cd = malloc(cd_size);
    entry_t *list = calloc(n, sizeof(entry_t));
    if (!cd || !list) {
        free(cd);
        free(list);
        set_err(err, err_len, "out of memory");
        return -1;
    }
    fseek(f, cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, f) != cd_size) {
        free(cd);
        free(list);
        set_err(err, err_len, "read error");
        return -1;
    }
    uint32_t pos = 0;
    int i = 0;
    for (; i < n; i++) {
        if (pos + 46 > cd_size || u32(cd + pos) != 0x02014b50u) {
            break;
        }
        uint16_t flags = u16(cd + pos + 8), nl = u16(cd + pos + 28), xl = u16(cd + pos + 30), cl = u16(cd + pos + 32);
        if (pos + 46 + nl + xl + cl > cd_size || nl >= sizeof(list[i].name) || (flags & 1)) {
            break;
        }
        list[i].method = u16(cd + pos + 10);
        list[i].crc = u32(cd + pos + 16);
        list[i].csize = u32(cd + pos + 20);
        list[i].usize = u32(cd + pos + 24);
        list[i].local_off = u32(cd + pos + 42);
        memcpy(list[i].name, cd + pos + 46, nl);
        pos += 46 + nl + xl + cl;
    }
    free(cd);
    if (i != n) {
        free(list);
        set_err(err, err_len, "damaged or encrypted ZIP");
        return -1;
    }
    *out = list;
    *count = n;
    return 0;
}

static bool safe_name(const char *n)
{
    return n[0] && n[0] != '/' && !strstr(n, "..") && !strchr(n, '\\');
}

/* Inflate (or copy) one entry; `sink` gets the output in chunks. */
typedef int (*sink_t)(const uint8_t *data, size_t n, void *user);

static int read_entry(FILE *f, const entry_t *en, sink_t sink, void *user, char *err, size_t err_len)
{
    uint8_t lh[30];
    if (fseek(f, en->local_off, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30 || u32(lh) != 0x04034b50u) {
        set_err(err, err_len, "bad local header");
        return -1;
    }
    if (fseek(f, en->local_off + 30 + u16(lh + 26) + u16(lh + 28), SEEK_SET) != 0) {
        set_err(err, err_len, "bad local header");
        return -1;
    }
    if (en->usize > MAX_ENTRY_SIZE) {
        set_err(err, err_len, "file too large");
        return -1;
    }
    uint8_t *in = malloc(4096), *outb = malloc(4096);
    if (!in || !outb) {
        free(in);
        free(outb);
        set_err(err, err_len, "out of memory");
        return -1;
    }
    int rc = 0;
    uint32_t crc = crc32(0, NULL, 0);
    uint32_t left = en->csize, produced = 0;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (en->method == 8 && inflateInit2(&zs, -15) != Z_OK) {
        free(in);
        free(outb);
        set_err(err, err_len, "zlib init failed");
        return -1;
    }
    if (en->method != 0 && en->method != 8) {
        rc = -1;
        set_err(err, err_len, "unsupported compression");
    }
    while (rc == 0 && left > 0) {
        size_t want = left < 4096 ? left : 4096;
        if (fread(in, 1, want, f) != want) {
            rc = -1;
            set_err(err, err_len, "read error");
            break;
        }
        left -= want;
        if (en->method == 0) {
            crc = crc32(crc, in, want);
            produced += want;
            if (sink(in, want, user) != 0) {
                rc = -1;
                set_err(err, err_len, "write error");
            }
        } else {
            zs.next_in = in;
            zs.avail_in = want;
            do {
                zs.next_out = outb;
                zs.avail_out = 4096;
                int z = inflate(&zs, Z_NO_FLUSH);
                if (z != Z_OK && z != Z_STREAM_END && z != Z_BUF_ERROR) {
                    rc = -1;
                    set_err(err, err_len, "inflate failed");
                    break;
                }
                size_t got = 4096 - zs.avail_out;
                if (got) {
                    crc = crc32(crc, outb, got);
                    produced += got;
                    if (produced > MAX_ENTRY_SIZE || sink(outb, got, user) != 0) {
                        rc = -1;
                        set_err(err, err_len, "write error or file too large");
                        break;
                    }
                }
                if (z == Z_STREAM_END) {
                    break;
                }
            } while (zs.avail_in > 0 || zs.avail_out == 0);
        }
    }
    if (en->method == 8) {
        inflateEnd(&zs);
    }
    free(in);
    free(outb);
    if (rc == 0 && (produced != en->usize || crc != en->crc)) {
        rc = -1;
        set_err(err, err_len, "checksum mismatch");
    }
    return rc;
}

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} mem_sink_t;

static int mem_sink(const uint8_t *data, size_t n, void *user)
{
    mem_sink_t *m = user;
    if (m->len + n + 1 > m->cap) {
        return -1;
    }
    memcpy(m->buf + m->len, data, n);
    m->len += n;
    return 0;
}

static int file_sink(const uint8_t *data, size_t n, void *user)
{
    return fwrite(data, 1, n, (FILE *)user) == n ? 0 : -1;
}

int phone_zip_read_file(const char *zip_path, const char *name, char **out, size_t *len, char *err, size_t err_len)
{
    FILE *f = fopen(zip_path, "rb");
    if (!f) {
        set_err(err, err_len, "cannot open the file");
        return -1;
    }
    entry_t *list = NULL;
    int n = 0, rc = -1;
    if (read_directory(f, &list, &n, err, err_len) == 0) {
        rc = -2;
        set_err(err, err_len, "file not in the archive");
        for (int i = 0; i < n; i++) {
            if (strcmp(list[i].name, name) == 0) {
                mem_sink_t m = {.cap = list[i].usize + 1};
                m.buf = malloc(m.cap);
                if (!m.buf) {
                    rc = -1;
                    set_err(err, err_len, "out of memory");
                    break;
                }
                rc = read_entry(f, &list[i], mem_sink, &m, err, err_len);
                if (rc == 0) {
                    m.buf[m.len] = 0;
                    *out = (char *)m.buf;
                    *len = m.len;
                } else {
                    free(m.buf);
                }
                break;
            }
        }
        free(list);
    }
    fclose(f);
    return rc;
}

static void make_parents(const char *path)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
}

int phone_zip_extract(const char *zip_path, const char *dest_dir, char *err, size_t err_len)
{
    FILE *f = fopen(zip_path, "rb");
    if (!f) {
        set_err(err, err_len, "cannot open the file");
        return -1;
    }
    entry_t *list = NULL;
    int n = 0, rc = read_directory(f, &list, &n, err, err_len);
    if (rc == 0) {
        uint32_t total = 0;
        for (int i = 0; i < n && rc == 0; i++) {
            total += list[i].usize;
            if (!safe_name(list[i].name) || total > MAX_TOTAL_SIZE) {
                rc = -1;
                set_err(err, err_len, "unsafe path or archive too large");
            }
        }
        mkdir(dest_dir, 0775);
        for (int i = 0; i < n && rc == 0; i++) {
            const char *name = list[i].name;
            if (strncmp(name, "META-INF/", 9) == 0) {
                continue;
            }
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", dest_dir, name);
            size_t nl = strlen(name);
            if (name[nl - 1] == '/') {
                make_parents(path);
                path[strlen(path) - 1] = 0;
                mkdir(path, 0775);
                continue;
            }
            make_parents(path);
            FILE *out = fopen(path, "wb");
            if (!out) {
                rc = -1;
                set_err(err, err_len, "cannot create a file");
                break;
            }
            rc = read_entry(f, &list[i], file_sink, out, err, err_len);
            fclose(out);
        }
        free(list);
    }
    fclose(f);
    return rc;
}
