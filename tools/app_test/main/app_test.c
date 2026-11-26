/* app_test: exercises every call of phone_elf_api_t (v3) and shows PASS / FAIL on the screen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "phone_elf_api.h"

static const phone_elf_api_t *A;
static phone_elf_ctx_t *C;

static int s_init_data = 1234;          /* .data */
static int s_zero[64];                  /* .bss */
static char s_text[1800];
static int s_pos;
static int s_pass, s_fail;
static void *s_click_label, *s_slider_label, *s_led_label, *s_tick_label, *s_switch_label, *s_life_label;
static int s_pauses, s_resumes;
static int s_clicks, s_led_step, s_ticks;
static uint32_t s_perms;

static void on_tick(void *user);
static void on_pause(void *user);
static void on_resume(void *user);
static void on_close_life(void *user);

static void check(const char *name, bool ok, const char *info)
{
    if (ok) s_pass++; else s_fail++;
    s_pos += snprintf(s_text + s_pos, sizeof(s_text) - s_pos, "%s %s%s%s\n", ok ? "PASS" : "FAIL", name,
                      info && info[0] ? " - " : "", info ? info : "");
    printf("app_test: %s %s %s\n", ok ? "PASS" : "FAIL", name, info ? info : "");
}

/* a call of a group: granted -> it works, not granted -> PHONE_ELF_E_PERM */
static bool perm_ok(uint32_t bit, int result)
{
    return (s_perms & bit) ? result >= 0 : result == PHONE_ELF_E_PERM;
}

static void run_tests(void)
{
    char info[112];
    info[0] = 0;

    snprintf(info, sizeof(info), "v%u size %u", (unsigned)A->version, (unsigned)A->size);
    check("api version", A->version == PHONE_ELF_API_VERSION && A->size == sizeof(phone_elf_api_t) && PHONE_ELF_HAS(A, free_sram), info);

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

    ok = s_init_data == 1234;
    for (int i = 0; i < 64; i++) if (s_zero[i]) ok = false;
    s_init_data++;
    check(".data / .bss", ok, "");

    s_perms = A->permissions(C);
    snprintf(info, sizeof(info), "mask 0x%02x", (unsigned)s_perms);
    check("permissions", s_perms <= 0x3f, info);

    /* files: libc on the data folder; make_dirs needs PERM_FILES */
    char path[128];
    snprintf(path, sizeof(path), "%s/test.txt", A->app_dir(C));
    int mk = A->make_dirs(C, path);
    if (s_perms & PHONE_ELF_PERM_FILES) {
        FILE *f = mk == 0 ? fopen(path, "wb") : NULL;
        ok = f != NULL;
        if (f) { fwrite("hello file", 1, 10, f); fclose(f); }
        char buf[16] = {0};
        f = ok ? fopen(path, "rb") : NULL;
        if (f) { ok = fread(buf, 1, 10, f) == 10 && memcmp(buf, "hello file", 10) == 0; fclose(f); } else ok = false;
        if (f) remove(path);
        check("files write/read", ok, path + (strlen(path) > 36 ? strlen(path) - 36 : 0));
    } else {
        check("files denied", mk == PHONE_ELF_E_PERM, "make_dirs refused");
    }

    /* the sandbox: the package folder is read only, everything outside the folders of the app is refused */
    char pk[160];
    snprintf(pk, sizeof(pk), "%s/manifest.json", A->pkg_dir(C));
    FILE *rf = fopen(pk, "rb");
    check("package file readable", rf != NULL, "pkg_dir()/manifest.json");
    if (rf) fclose(rf);
    snprintf(pk, sizeof(pk), "%s/sandbox.tmp", A->pkg_dir(C));
    FILE *wf = fopen(pk, "wb");
    check("package folder read only", wf == NULL, "fopen(wb) refused");
    if (wf) { fclose(wf); remove(pk); }
    FILE *of = fopen("/sdcard/packages/app_test.bpk", "rb");
    check("outside files refused", of == NULL, "fopen of another folder refused");
    if (of) fclose(of);
    snprintf(pk, sizeof(pk), "%s/manifest.json", A->pkg_dir(C));
    int rm = remove(pk);
    FILE *again = fopen(pk, "rb");
    check("remove refused", rm != 0 && again != NULL, "the manifest is still there");
    if (again) fclose(again);

    /* libm, 64-bit integers and doubles come from the chosen symbol list */
    volatile double d = 2.0;
    volatile long long big = 1000000000000LL;
    double q = sqrt(d) * sqrt(d);
    long long div = big / 7;
    snprintf(info, sizeof(info), "q=%d.%03d div=%d", (int)q, (int)((q - (int)q) * 1000), (int)(div / 1000000));
    check("math, 64-bit", fabs(q - 2.0) < 0.001 && div == 142857142857LL && fabsf(sinf(1.5707963f) - 1.0f) < 0.001f, info);

    /* settings: a 32-bit value, kept per app */
    ok = A->pref_set(C, "t1", 123456789) == 0 && A->pref_get(C, "t1", -1) == 123456789 && A->pref_get(C, "nokey", 77) == 77;
    check("pref 32-bit", ok, "");

    uint32_t t0 = A->millis(C);
    A->delay_ms(C, 50);
    uint32_t dt = A->millis(C) - t0;
    snprintf(info, sizeof(info), "50 ms = %u ms, epoch %d", (unsigned)dt, (int)A->epoch(C));
    check("time", dt >= 45 && dt < 200, info);

    const char *t = A->tr(C, "xin chao", "hello");
    check("tr", t && (strcmp(t, "xin chao") == 0 || strcmp(t, "hello") == 0), t);

    snprintf(info, sizeof(info), "PSRAM %u KB, SRAM %u KB", (unsigned)(A->free_psram(C) / 1024), (unsigned)(A->free_sram(C) / 1024));
    check("memory info", A->free_psram(C) > 0 && A->free_sram(C) > 0, info);

    /* the groups of permissions */
    phone_elf_imu_t imu;
    int r = A->imu_read(C, &imu);
    snprintf(info, sizeof(info), "r=%d", r);
    check("perm imu", perm_ok(PHONE_ELF_PERM_IMU, r), info);
    phone_elf_battery_t bat;
    r = A->battery_read(C, &bat);
    snprintf(info, sizeof(info), "r=%d", r);
    check("perm battery", perm_ok(PHONE_ELF_PERM_BATTERY, r), info);
    r = A->net_online(C);
    snprintf(info, sizeof(info), "r=%d", r);
    check("perm net", perm_ok(PHONE_ELF_PERM_NET, r), info);
    r = A->volume_get(C);
    snprintf(info, sizeof(info), "r=%d", r);
    check("perm audio", perm_ok(PHONE_ELF_PERM_AUDIO, r), info);
    r = A->led_off(C);
    snprintf(info, sizeof(info), "r=%d", r);
    check("perm led", perm_ok(PHONE_ELF_PERM_LED, r), info);

    /* life cycle: pause / resume / close handlers (timers pause by themselves in the background) */
    static const phone_elf_lifecycle_t lc = {.on_pause = on_pause, .on_resume = on_resume, .on_close = on_close_life, .user = 0, .flags = 0};
    check("lifecycle set", A->lifecycle_set(C, &lc) == 0 && A->is_foreground(C), "handlers registered, foreground");

    /* UI helpers the screen below uses */
    check("timer create", A->timer_create(C, 500, on_tick, 0) != NULL, "every 500 ms, the host deletes it on close");
}

static void on_beep(void *e)
{
    (void)e;
    s_clicks++;
    int r = A->beep(C);
    char b[64]; snprintf(b, sizeof(b), "clicks: %d (beep %d)", s_clicks, r);
    A->label_set_text(C, s_click_label, b);
}
static void on_led(void *e)
{
    (void)e;
    s_clicks++;
    static const uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    static const char *names[] = {"red blink", "green solid", "blue breathe", "rainbow"};
    int s = s_led_step++ % 4;
    int r = A->led_set(C, colors[s], s == 0 ? PHONE_ELF_LED_BLINK : s == 1 ? PHONE_ELF_LED_SOLID : s == 2 ? PHONE_ELF_LED_BREATHE : PHONE_ELF_LED_RAINBOW, 1200);
    char b[64];
    if (r == 0) snprintf(b, sizeof(b), "LED: %s", names[s]); else snprintf(b, sizeof(b), "LED: refused (%d)", r);
    A->label_set_text(C, s_led_label, b);
}
static void on_led_off(void *e) { (void)e; int r = A->led_off(C); A->label_set_text(C, s_led_label, r == 0 ? "LED: off" : "LED: refused"); }
static void on_slider(void *e)
{
    int v = A->slider_get_value(C, A->event_target(e));
    char b[48]; snprintf(b, sizeof(b), "slider: %d (user data %s)", v, (const char *)A->event_user_data(e));
    A->label_set_text(C, s_slider_label, b);
}
static void on_switch(void *e)
{
    bool on = A->switch_get_state(C, A->event_target(e));
    A->label_set_text(C, s_switch_label, on ? "switch: on" : "switch: off");
}
static void on_tick(void *user)
{
    (void)user;
    char b[48]; snprintf(b, sizeof(b), "timer ticks: %d", ++s_ticks);
    A->label_set_text(C, s_tick_label, b);
}
static void show_life(void)
{
    char b[96];
    snprintf(b, sizeof(b), "pauses %d, resumes %d, closes saved %d (ticks %d at the last close)", s_pauses, s_resumes,
             (int)A->pref_get(C, "closes", 0), (int)A->pref_get(C, "lastticks", 0));
    if (s_life_label) A->label_set_text(C, s_life_label, b);
}
static void on_pause(void *user)
{
    (void)user;
    s_pauses++;
    printf("app_test: on_pause ticks=%d foreground=%d\n", s_ticks, A->is_foreground(C));
    show_life();
}
static void on_resume(void *user)
{
    (void)user;
    s_resumes++;
    printf("app_test: on_resume ticks=%d foreground=%d\n", s_ticks, A->is_foreground(C));
    show_life();
}
static void on_close_life(void *user)
{
    (void)user;
    int n = A->pref_get(C, "closes", 0) + 1;
    A->pref_set(C, "closes", n);
    A->pref_set(C, "lastticks", s_ticks);
    printf("app_test: on_close ticks=%d saved closes=%d\n", s_ticks, n);
}
static void on_close(void *e) { (void)e; A->close(C); }

static void *button(void *parent, const char *text, phone_elf_event_cb_t cb)
{
    void *b = A->button_create(C, parent);
    A->obj_set_size(C, b, -45, 64);
    A->obj_add_event_cb(C, b, cb, PHONE_ELF_EV_CLICKED, 0);
    void *l = A->label_create(C, b);
    A->label_set_text(C, l, text);
    A->obj_align(C, l, PHONE_ELF_ALIGN_CENTER, 0, 0);
    return b;
}

int main(int argc, char *argv[])
{
    if (argc < 2) return -1;
    A = (const phone_elf_api_t *)argv[0];
    C = (phone_elf_ctx_t *)argv[1];
    if (!A || !C || A->version != PHONE_ELF_API_VERSION) return -1;
    printf("app_test: start, api v%u\n", (unsigned)A->version);

    run_tests();

    void *root = A->root(C);
    A->obj_set_flex_column(C, root);

    void *title = A->label_create(C, root);
    char b[96];
    snprintf(b, sizeof(b), "%s   PASS %d   FAIL %d", A->app_dir(C) + strlen("/sdcard/appdata/"), s_pass, s_fail);
    A->label_set_text(C, title, b);
    A->obj_set_text_color(C, title, s_fail ? 0xFF6060 : 0x60FF60);

    void *res = A->label_create(C, root);
    A->label_set_text(C, res, s_text);
    A->obj_set_size(C, res, -100, -1);

    s_tick_label = A->label_create(C, root);
    A->label_set_text(C, s_tick_label, "timer ticks: 0");
    s_click_label = A->label_create(C, root);
    A->label_set_text(C, s_click_label, "clicks: 0");
    s_led_label = A->label_create(C, root);
    A->label_set_text(C, s_led_label, "LED: off");
    s_slider_label = A->label_create(C, root);
    A->label_set_text(C, s_slider_label, "slider: 0");
    s_switch_label = A->label_create(C, root);
    A->label_set_text(C, s_switch_label, "switch: off");
    s_life_label = A->label_create(C, root);
    show_life();

    void *sl = A->slider_create(C, root);
    A->obj_set_size(C, sl, -90, 20);
    A->slider_set_range(C, sl, 0, 100);
    A->slider_set_value(C, sl, 25);
    A->obj_add_event_cb(C, sl, on_slider, PHONE_ELF_EV_VALUE_CHANGED, (void *)"ok");

    void *sw = A->switch_create(C, root);
    A->obj_add_event_cb(C, sw, on_switch, PHONE_ELF_EV_VALUE_CHANGED, 0);

    void *row = A->obj_create(C, root);
    A->obj_set_size(C, row, -100, 80);
    A->obj_set_flex_row(C, row);
    button(row, "Beep", on_beep);
    button(row, "LED next", on_led);
    void *row2 = A->obj_create(C, root);
    A->obj_set_size(C, row2, -100, 80);
    A->obj_set_flex_row(C, row2);
    button(row2, "LED off", on_led_off);
    button(row2, "Close", on_close);
    return s_fail;
}
