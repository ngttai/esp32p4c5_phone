/*
 * Runs ELF apps (phone_elf_api_t, API v3) on screens of the phone: several at a time, each with its own image, context
 * and permissions. Everything here is called with the LVGL lock held (from an app's run() / close()).
 */
#pragma once
#include <stddef.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct phone_elf_inst phone_elf_inst_t;
typedef void (*phone_elf_host_close_cb_t)(void *user);

#define PHONE_ELF_HOST_MAX_APPS 4

/**
 * Load `elf_path`, give it a full-size container on `screen` and run its entry.
 * `perms` = PHONE_ELF_PERM_* the manifest asked for. `close_cb(user)` is called (later, on the LVGL task) when the app asks to close.
 * Returns the instance, or NULL with the reason in `err`.
 */
phone_elf_inst_t *phone_elf_host_open(const char *app_id, const char *elf_path, unsigned perms, lv_obj_t *screen,
                                      phone_elf_host_close_cb_t close_cb, void *user, char *err, size_t err_len);

/** Delete its timers and container, let the LED go, unload the ELF, free the instance. NULL is fine. */
void phone_elf_host_close(phone_elf_inst_t *inst);

/** Number of apps loaded now. */
int phone_elf_host_count(void);

/** "led,audio,imu" -> bit mask; unknown names are ignored (and counted in *unknown when it is not NULL). */
unsigned phone_elf_host_parse_permission(const char *name);

#ifdef __cplusplus
}
#endif
