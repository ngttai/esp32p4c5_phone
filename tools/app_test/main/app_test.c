/* app_test: exercises every call of phone_elf_api_t and shows PASS / FAIL on the screen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "phone_elf_api.h"

static const phone_elf_api_t *A;

static int s_init_data = 1234;          /* .data */
static int s_zero[64];                  /* .bss */
static char s_text[1400];
static int s_pos;
static int s_pass, s_fail;
static void *s_result_label, *s_click_label, *s_slider_label, *s_led_label;
static int s_clicks, s_led_step;

static void check(const char *name, bool ok, const char *info)
{
    if (ok) s_pass++; else s_fail++;
    s_pos += snprintf(s_text + s_pos, sizeof(s_text) - s_pos, "%s %s%s%s\n", ok ? "PASS" : "FAIL", name,
                      info && info[0] ? " - " : "", info ? info : "");
    printf("app_test: %s %s %s\n", ok ? "PASS" : "FAIL", name, info ? info : "");
}

static void run_tests(void)
{
    char info[96];
    info[0] = 0;

    snprintf(info, sizeof(info), "v%u", (unsigned)A->version);
    check("api version", A->version == PHONE_ELF_API_VERSION, info);

    /* libc through the loader's tables and the host's extra table */
    bool ok = strlen("abc") == 3 && strcmp("a", "b") < 0 && memcmp("xyz", "xyz", 3) == 0 && atoi("4242") == 4242;
    check("libc string", ok, "strlen strcmp memcmp atoi");
    uint8_t *m = malloc(65536);
    ok = m != NULL;
    if (m) {
        for (int i = 0; i < 65536; i++) m[i] = (uint8_t)(i * 7);
        for (int i = 0; i < 65536; i++) if (m[i] != (uint8_t)(i * 7)) ok = false;
        free(m);
    }
    check("malloc 64 KB", ok, "");

    /* the sections of the image */
    ok = s_init_data == 1234;
    for (int i = 0; i < 64; i++) if (s_zero[i]) ok = false;
    s_init_data++;
    check(".data / .bss", ok, "");

    /* files, with libc calls and the app folder */
    char path[128];
    snprintf(path, sizeof(path), "%s/test.txt", A->app_dir());
    ok = A->make_dirs(path) == 0;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    ok = f != NULL;
    if (f) { fwrite("hello file", 1, 10, f); fclose(f); }
    char buf[16] = {0};
    f = ok ? fopen(path, "rb") : NULL;
    if (f) { ok = fread(buf, 1, 10, f) == 10 && memcmp(buf, "hello file", 10) == 0; fclose(f); } else ok = false;
    if (f) remove(path);
    check("file write/read", ok, path + (strlen(path) > 40 ? strlen(path) - 40 : 0));

    /* settings */
    A->pref_set("t1", 201);
    check("pref set/get", A->pref_get("t1", -1) == 201 && A->pref_get("nokey", 77) == 77, "");

    /* time */
    uint32_t t0 = A->millis();
    A->delay_ms(50);
    uint32_t dt = A->millis() - t0;
    snprintf(info, sizeof(info), "50 ms = %u ms, epoch %d", (unsigned)dt, (int)A->epoch());
    check("time", dt >= 45 && dt < 200, info);

    const char *t = A->tr("xin chao", "hello");
    check("tr", t && (strcmp(t, "xin chao") == 0 || strcmp(t, "hello") == 0), t);

    snprintf(info, sizeof(info), "PSRAM %u KB, SRAM %u KB", (unsigned)(A->free_psram() / 1024), (unsigned)(A->free_sram() / 1024));
    check("memory info", A->free_psram() > 0 && A->free_sram() > 0, info);

    phone_elf_imu_t imu;
    int r = A->imu_read(&imu);
    snprintf(info, sizeof(info), "r=%d acc %d %d %d", r, imu.accel[0], imu.accel[1], imu.accel[2]);
    check("imu", r == 0 && (imu.accel[0] || imu.accel[1] || imu.accel[2]), info);

    phone_elf_battery_t bat;
    r = A->battery_read(&bat);
    snprintf(info, sizeof(info), "%d%% %d mV usb %d", bat.percent, bat.voltage_mv, bat.charging_usb);
    check("battery", r == 0, info);

    snprintf(info, sizeof(info), "online=%d, volume %d", A->net_online(), A->volume_get());
    check("net, volume", A->volume_get() >= 0, info);
}

static void on_beep(void *e) { (void)e; s_clicks++; A->beep(); char b[48]; snprintf(b, sizeof(b), "clicks: %d", s_clicks); A->label_set_text(s_click_label, b); }
static void on_led(void *e)
{
    (void)e;
    s_clicks++;
    static const uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    static const char *names[] = {"red blink", "green solid", "blue breathe", "rainbow"};
    int s = s_led_step++ % 4;
    A->led_set(colors[s], s == 0 ? PHONE_ELF_LED_BLINK : s == 1 ? PHONE_ELF_LED_SOLID : s == 2 ? PHONE_ELF_LED_BREATHE : PHONE_ELF_LED_RAINBOW, 1200);
    char b[64]; snprintf(b, sizeof(b), "LED: %s", names[s]); A->label_set_text(s_led_label, b);
}
static void on_led_off(void *e) { (void)e; A->led_off(); A->label_set_text(s_led_label, "LED: off"); }
static void on_slider(void *e)
{
    int v = A->slider_get_value(A->event_target(e));
    char b[48]; snprintf(b, sizeof(b), "slider: %d (user data %s)", v, (const char *)A->event_user_data(e));
    A->label_set_text(s_slider_label, b);
}
static void on_close(void *e) { (void)e; A->close(); }

static void *button(void *parent, const char *text, phone_elf_event_cb_t cb)
{
    void *b = A->button_create(parent);
    A->obj_set_size(b, -45, 64);
    A->obj_add_event_cb(b, cb, PHONE_ELF_EV_CLICKED, 0);
    void *l = A->label_create(b);
    A->label_set_text(l, text);
    A->obj_align(l, PHONE_ELF_ALIGN_CENTER, 0, 0);
    return b;
}

int main(int argc, char *argv[])
{
    A = argc > 0 ? (const phone_elf_api_t *)argv[0] : 0;
    if (!A || A->version != PHONE_ELF_API_VERSION) return -1;
    printf("app_test: start, api v%u\n", (unsigned)A->version);

    run_tests();

    void *root = A->root();
    A->obj_set_flex_column(root);
    A->obj_set_text_color(root, 0xFFFFFF);

    void *title = A->label_create(root);
    char b[96];
    snprintf(b, sizeof(b), "app_test.elf   PASS %d   FAIL %d", s_pass, s_fail);
    A->label_set_text(title, b);
    A->obj_set_text_color(title, s_fail ? 0xFF6060 : 0x60FF60);

    s_result_label = A->label_create(root);
    A->label_set_text(s_result_label, s_text);
    A->obj_set_size(s_result_label, -100, -1);

    s_click_label = A->label_create(root);
    A->label_set_text(s_click_label, "clicks: 0");
    s_led_label = A->label_create(root);
    A->label_set_text(s_led_label, "LED: off");
    s_slider_label = A->label_create(root);
    A->label_set_text(s_slider_label, "slider: 0");

    void *sl = A->slider_create(root);
    A->obj_set_size(sl, -90, 20);
    A->slider_set_range(sl, 0, 100);
    A->slider_set_value(sl, 25);
    A->obj_add_event_cb(sl, on_slider, PHONE_ELF_EV_VALUE_CHANGED, (void *)"ok");

    void *row = A->obj_create(root);
    A->obj_set_size(row, -100, 80);
    A->obj_set_flex_row(row);
    button(row, "Beep", on_beep);
    button(row, "LED next", on_led);
    void *row2 = A->obj_create(root);
    A->obj_set_size(row2, -100, 80);
    A->obj_set_flex_row(row2);
    button(row2, "LED off", on_led_off);
    button(row2, "Close", on_close);
    return s_fail;
}
