/* MirrorTV - C wrapper around Apple's open-source ALAC decoder. GPL-3.0-or-later */
#ifndef MIRRORTV_ALAC_WRAPPER_H
#define MIRRORTV_ALAC_WRAPPER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct alac_wrapper alac_wrapper_t;

/* AirPlay ALAC: 44100 Hz, 16 bit, stereo, 352 frames per packet */
alac_wrapper_t *alac_wrapper_create(void);
/* returns number of interleaved stereo frames written to out (s16), or <0 on error */
int alac_wrapper_decode(alac_wrapper_t *w, const uint8_t *in, int in_len, int16_t *out, int out_max_frames);
void alac_wrapper_destroy(alac_wrapper_t *w);

#ifdef __cplusplus
}
#endif
#endif
