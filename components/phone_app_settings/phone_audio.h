#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Speaker volume in percent (0 = silent); used by the next sound played. */
void phone_audio_set_volume(int percent);
int phone_audio_get_volume(void);
/** Play a short tone at the current volume so the user hears the setting; returns immediately. */
void phone_audio_beep(void);

/**
 * Take the speaker for a long playback (music). While it is taken, phone_audio_beep() stays silent.
 * Returns the codec handle (not opened), or NULL when the speaker is busy or not available.
 * The volume setting is still phone_audio_get_volume(): the player applies it.
 */
void *phone_audio_speaker_take(void);
void phone_audio_speaker_give(void);

#ifdef __cplusplus
}
#endif
