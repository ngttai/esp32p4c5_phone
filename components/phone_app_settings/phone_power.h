#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHONE_POWER_RESTART = 0,       /*!< "Restart?" */
    PHONE_POWER_RESTART_LANG = 1,  /*!< "Restart to apply language?" */
} phone_power_action_t;

/** Show the restart confirm dialog on the top layer; call from LVGL context. */
void phone_power_confirm(phone_power_action_t action);

#ifdef __cplusplus
}
#endif
