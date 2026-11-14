#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Speaker volume in percent (0 = silent); used by the next sound played. */
void phone_audio_set_volume(int percent);
int phone_audio_get_volume(void);
/** Play a short tone at the current volume so the user hears the setting; returns immediately. */
void phone_audio_beep(void);

#ifdef __cplusplus
}
#endif
