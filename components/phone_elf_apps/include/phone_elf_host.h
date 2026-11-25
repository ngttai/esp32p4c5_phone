/*
 * Runs one ELF app (phone_elf_api_t) on a screen of the phone. One at a time. Everything here is called with the
 * LVGL lock held (from an app's run() / close()).
 */
#pragma once
#include <stddef.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*phone_elf_host_close_cb_t)(void *user);

/** Load `elf_path`, give it a full-size container on `screen` and run its entry. 0 on success, -2 when another ELF app is open. */
int phone_elf_host_open(const char *app_id, const char *elf_path, lv_obj_t *screen,
                        phone_elf_host_close_cb_t close_cb, void *user, char *err, size_t err_len);

/** Delete the container, let the LED go, unload the ELF. Safe when nothing is open. */
void phone_elf_host_close(void);

bool phone_elf_host_is_open(void);

#ifdef __cplusplus
}
#endif
