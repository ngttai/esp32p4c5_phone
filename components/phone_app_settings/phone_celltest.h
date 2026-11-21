#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 4G modem probe (Settings > Debug > 4G), stage 0 of PLAN_4G.md: powers the ML307 (it runs from the
 * battery), then identity, SIM and network, the baud rates the level-shifted UART carries, CMUX and a
 * PPP dial (ATD*99#, hung up at once). Every command and answer goes to the log (tag CELL) and to a
 * text the page shows. The modem is switched off at the end. Runs in its own task.
 */
bool phone_celltest_start(void);
bool phone_celltest_running(void);
/** The last `len - 1` characters of the report */
void phone_celltest_text(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
