/* MirrorTV - hardware H.264 decoding to an Android Surface. GPL-3.0-or-later */
#ifndef MIRRORTV_VIDEO_DECODER_H
#define MIRRORTV_VIDEO_DECODER_H

#include <stdint.h>
#include <android/native_window.h>

typedef void (*video_size_callback_t)(int width, int height);
typedef void (*video_first_frame_callback_t)(void);

void video_decoder_init(video_size_callback_t on_size, video_first_frame_callback_t on_first_frame);
/* window may be NULL; ownership of one reference is taken */
void video_decoder_set_window(ANativeWindow *window);
/* Annex-B access unit (4-byte start codes), may begin with SPS+PPS */
void video_decoder_push(const uint8_t *data, int len, uint64_t pts_us);
void video_decoder_flush(void);
/* tear down the codec; it is recreated on the next SPS */
void video_decoder_reset(void);
void video_decoder_shutdown(void);

#endif
