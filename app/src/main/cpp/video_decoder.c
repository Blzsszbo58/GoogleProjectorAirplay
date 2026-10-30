/*
 * MirrorTV - hardware H.264 decoding (NDK AMediaCodec) straight onto a Surface.
 *
 * The AirPlay mirror stream arrives as complete Annex-B access units; UxPlay
 * prepends the SPS+PPS to each key frame.  The codec is (re)created whenever a
 * new SPS shows up (the iPhone sends a new one when it rotates), using the
 * SPS/PPS as csd-0/csd-1.  A separate thread drains the output queue and
 * renders frames as soon as they are decoded (lowest latency).
 *
 * License: GPL-3.0-or-later
 */

#include "video_decoder.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "MirrorTV-video"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MAX_PARAM_SET 512
#define INPUT_TIMEOUT_US 100000      /* wait at most 100 ms for a free input buffer */
#define OUTPUT_TIMEOUT_US 20000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static AMediaCodec *codec = NULL;
static ANativeWindow *window = NULL;
static uint8_t sps[MAX_PARAM_SET], pps[MAX_PARAM_SET];
static int sps_len = 0, pps_len = 0;
static bool need_keyframe = true;
static bool first_frame_rendered = false;

static pthread_t output_thread;
static bool output_thread_running = false;
static volatile bool output_thread_stop = false;

static video_size_callback_t size_cb = NULL;
static video_first_frame_callback_t first_frame_cb = NULL;

/* ---------- Annex-B helpers ---------- */

/* returns pointer to the NAL payload after the next start code at or after p, or NULL */
static const uint8_t *next_nal(const uint8_t *p, const uint8_t *end, int *sc_len) {
    while (p + 3 <= end) {
        if (p[0] == 0 && p[1] == 0) {
            if (p[2] == 1) {
                *sc_len = 3;
                return p + 3;
            }
            if (p + 4 <= end && p[2] == 0 && p[3] == 1) {
                *sc_len = 4;
                return p + 4;
            }
        }
        p++;
    }
    return NULL;
}

/* Extract SPS/PPS (including a 4-byte start code) from an access unit. Returns true if both found. */
static bool find_param_sets(const uint8_t *data, int len,
                            uint8_t *out_sps, int *out_sps_len, uint8_t *out_pps, int *out_pps_len,
                            bool *has_idr) {
    const uint8_t *end = data + len;
    int sc = 0;
    const uint8_t *nal = next_nal(data, end, &sc);
    bool got_sps = false, got_pps = false;
    *has_idr = false;
    while (nal && nal < end) {
        int next_sc = 0;
        const uint8_t *next = next_nal(nal, end, &next_sc);
        const uint8_t *nal_end = next ? next - next_sc : end;
        int nal_size = (int) (nal_end - nal);
        int type = nal[0] & 0x1f;
        if (type == 7 && nal_size + 4 <= MAX_PARAM_SET) {
            memcpy(out_sps, "\0\0\0\1", 4);
            memcpy(out_sps + 4, nal, nal_size);
            *out_sps_len = nal_size + 4;
            got_sps = true;
        } else if (type == 8 && nal_size + 4 <= MAX_PARAM_SET) {
            memcpy(out_pps, "\0\0\0\1", 4);
            memcpy(out_pps + 4, nal, nal_size);
            *out_pps_len = nal_size + 4;
            got_pps = true;
        } else if (type == 5) {
            *has_idr = true;
        }
        nal = next;
    }
    return got_sps && got_pps;
}

/* ---------- codec lifecycle (call with lock held) ---------- */

static void stop_output_thread_locked(void) {
    if (output_thread_running) {
        output_thread_stop = true;
        pthread_mutex_unlock(&lock);
        pthread_join(output_thread, NULL);
        pthread_mutex_lock(&lock);
        output_thread_running = false;
    }
}

static void destroy_codec_locked(void) {
    stop_output_thread_locked();
    if (codec) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        codec = NULL;
        LOGI("decoder destroyed");
    }
    need_keyframe = true;
}

static void *output_loop(void *arg) {
    (void) arg;
    AMediaCodec *c;
    pthread_mutex_lock(&lock);
    c = codec;
    pthread_mutex_unlock(&lock);

    while (!output_thread_stop) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(c, &info, OUTPUT_TIMEOUT_US);
        if (idx >= 0) {
            bool render = info.size > 0 || !(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            AMediaCodec_releaseOutputBuffer(c, (size_t) idx, render);
            if (render && !first_frame_rendered) {
                first_frame_rendered = true;
                if (first_frame_cb) first_frame_cb();
            }
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *fmt = AMediaCodec_getOutputFormat(c);
            int32_t w = 0, h = 0, l = 0, t = 0, r = -1, b = -1;
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
            if (AMediaFormat_getInt32(fmt, "crop-left", &l) &&
                AMediaFormat_getInt32(fmt, "crop-top", &t) &&
                AMediaFormat_getInt32(fmt, "crop-right", &r) &&
                AMediaFormat_getInt32(fmt, "crop-bottom", &b)) {
                w = r - l + 1;
                h = b - t + 1;
            }
            AMediaFormat_delete(fmt);
            LOGI("output format: %dx%d", w, h);
            if (size_cb && w > 0 && h > 0) size_cb(w, h);
        }
        /* INFO_TRY_AGAIN_LATER / OUTPUT_BUFFERS_CHANGED: just loop */
    }
    return NULL;
}

static bool create_codec_locked(void) {
    if (!window) return false;
    codec = AMediaCodec_createDecoderByType("video/avc");
    if (!codec) {
        LOGE("no H.264 decoder available");
        return false;
    }
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(fmt, "max-width", 1920);
    AMediaFormat_setInt32(fmt, "max-height", 1920);
    AMediaFormat_setInt32(fmt, "low-latency", 1);          /* Android 11+, ignored before */
    AMediaFormat_setInt32(fmt, "priority", 0);             /* realtime */
    AMediaFormat_setBuffer(fmt, "csd-0", sps, (size_t) sps_len);
    AMediaFormat_setBuffer(fmt, "csd-1", pps, (size_t) pps_len);

    media_status_t st = AMediaCodec_configure(codec, fmt, window, NULL, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        LOGE("AMediaCodec_configure failed: %d", st);
        AMediaCodec_delete(codec);
        codec = NULL;
        return false;
    }
    st = AMediaCodec_start(codec);
    if (st != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed: %d", st);
        AMediaCodec_delete(codec);
        codec = NULL;
        return false;
    }
    output_thread_stop = false;
    if (pthread_create(&output_thread, NULL, output_loop, NULL) == 0) {
        output_thread_running = true;
    }
    LOGI("decoder started");
    return true;
}

/* ---------- public API ---------- */

void video_decoder_init(video_size_callback_t on_size, video_first_frame_callback_t on_first_frame) {
    size_cb = on_size;
    first_frame_cb = on_first_frame;
}

void video_decoder_set_window(ANativeWindow *new_window) {
    pthread_mutex_lock(&lock);
    destroy_codec_locked();     /* the codec is bound to the old surface */
    if (window) ANativeWindow_release(window);
    window = new_window;
    first_frame_rendered = false;
    /* if we already have parameter sets, the next key frame recreates the codec */
    pthread_mutex_unlock(&lock);
}

void video_decoder_push(const uint8_t *data, int len, uint64_t pts_us) {
    uint8_t new_sps[MAX_PARAM_SET], new_pps[MAX_PARAM_SET];
    int new_sps_len = 0, new_pps_len = 0;
    bool has_idr = false;
    bool has_params = find_param_sets(data, len, new_sps, &new_sps_len, new_pps, &new_pps_len, &has_idr);

    pthread_mutex_lock(&lock);
    if (has_params) {
        bool changed = new_sps_len != sps_len || memcmp(new_sps, sps, (size_t) sps_len) != 0 ||
                       new_pps_len != pps_len || memcmp(new_pps, pps, (size_t) pps_len) != 0;
        if (changed || !codec) {
            if (codec) {
                LOGI("stream parameters changed, recreating decoder");
                destroy_codec_locked();
            }
            memcpy(sps, new_sps, (size_t) new_sps_len);
            sps_len = new_sps_len;
            memcpy(pps, new_pps, (size_t) new_pps_len);
            pps_len = new_pps_len;
            create_codec_locked();
        }
    }
    if (has_params || has_idr) need_keyframe = false;
    if (!codec || need_keyframe) {
        pthread_mutex_unlock(&lock);
        return;
    }

    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, INPUT_TIMEOUT_US);
    if (idx < 0) {
        LOGW("decoder input full, dropping one frame");
        pthread_mutex_unlock(&lock);
        return;
    }
    size_t cap = 0;
    uint8_t *buf = AMediaCodec_getInputBuffer(codec, (size_t) idx, &cap);
    if (!buf || (size_t) len > cap) {
        LOGE("input buffer too small (%zu < %d)", cap, len);
        AMediaCodec_queueInputBuffer(codec, (size_t) idx, 0, 0, pts_us, 0);
        pthread_mutex_unlock(&lock);
        return;
    }
    memcpy(buf, data, (size_t) len);
    AMediaCodec_queueInputBuffer(codec, (size_t) idx, 0, (size_t) len, pts_us, 0);
    pthread_mutex_unlock(&lock);
}

void video_decoder_flush(void) {
    pthread_mutex_lock(&lock);
    if (codec) {
        AMediaCodec_flush(codec);
        need_keyframe = true;
    }
    pthread_mutex_unlock(&lock);
}

void video_decoder_reset(void) {
    pthread_mutex_lock(&lock);
    destroy_codec_locked();
    sps_len = pps_len = 0;
    first_frame_rendered = false;
    pthread_mutex_unlock(&lock);
}

void video_decoder_shutdown(void) {
    pthread_mutex_lock(&lock);
    destroy_codec_locked();
    if (window) {
        ANativeWindow_release(window);
        window = NULL;
    }
    sps_len = pps_len = 0;
    pthread_mutex_unlock(&lock);
}
