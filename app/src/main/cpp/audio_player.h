/* MirrorTV - AirPlay audio decoding and playback (AAudio). GPL-3.0-or-later */
#ifndef MIRRORTV_AUDIO_PLAYER_H
#define MIRRORTV_AUDIO_PLAYER_H
#include <stdint.h>

/* AirPlay compression types (ct) */
#define AUDIO_CT_PCM     1
#define AUDIO_CT_ALAC    2
#define AUDIO_CT_AAC_LC  4
#define AUDIO_CT_AAC_ELD 8

/* (re)start playback for the given compression type */
void audio_player_start(int ct);
void audio_player_push(const uint8_t *data, int len);
void audio_player_flush(void);
/* linear gain 0.0 .. 1.0 */
void audio_player_set_volume(float gain);
void audio_player_stop(void);

#endif
