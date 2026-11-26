/* hello: the smallest useful app. A counter button that keeps its value in the settings of the app, an uptime that stops
 * while the app is in the background, and a Close button. No permission needed. */
#include "phone_elf_sdk.h"

static void *s_count_label, *s_time_label;
static int s_count;
static uint32_t s_started;

static void show_count(void)
{
    char b[64];
    snprintf(b, sizeof(b), "%s: %d", app_tr("Số lần bấm", "Taps"), s_count);
    ui_text(s_count_label, b);
}

static void on_tap(void *event)
{
    (void)event;
    s_count++;
    pref_save("count", s_count);
    show_count();
}

static void on_tick(void *user)
{
    (void)user;
    char b[64];
    snprintf(b, sizeof(b), "%s: %u s", app_tr("Đang chạy", "Running"), (unsigned)((app_millis() - s_started) / 1000));
    ui_text(s_time_label, b);
}

static void on_close_life(void *user)
{
    (void)user;
    app_logf("closing with %d taps", s_count);       /* the settings are written on every tap; this is the place for anything else */
}

static void on_quit(void *event)
{
    (void)event;
    app_close();
}

int main(int argc, char *argv[])
{
    if (phone_init(argc, argv) != 0) return -1;

    void *root = ui_root();
    ui_column(root);
    void *title = ui_label(root);
    ui_text(title, app_tr("Xin chào từ một ứng dụng ELF", "Hello from an ELF app"));
    ui_color(title, 0x60D0FF);
    ui_font_size(title, 22);

    s_count = pref_int("count", 0);
    s_count_label = ui_label(root);
    show_count();
    s_time_label = ui_label(root);
    s_started = app_millis();
    timer_start(1000, on_tick, 0);                    /* paused by the host while the app is in the background */
    on_tick(0);

    ui_button(root, app_tr("Bấm thử", "Tap me"), on_tap, 0);
    ui_button(root, app_tr("Đóng", "Close"), on_quit, 0);
    app_lifecycle(0, 0, on_close_life, 0, 0);
    return 0;
}
