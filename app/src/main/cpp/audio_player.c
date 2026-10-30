/*
 * MirrorTV - AirPlay audio: decode (AAC-ELD / AAC-LC via AMediaCodec, ALAC via
 * Apple's decoder, raw PCM) and play through AAudio.
 *
 * Decoded PCM goes into a ring buffer that the AAudio data callback drains.
 * If the buffer grows beyond MAX_LATENCY_MS (e.g. the clocks drift apart) the
 * oldest audio is dropped so latency stays bounded.
 *
 * License: GPL-3.0-or-later
 */

#include "audio_player.h"
#include "alac_wrapper.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define TAG "MirrorTV-audio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SAMPLE_RATE 44100
#define CHANNELS 2
#define RING_FRAMES (SAMPLE_RATE * 2)              /* 2 s capacity */
#define MAX_LATENCY_FRAMES (SAMPLE_RATE * 3 / 4)   /* drop audio beyond 750 ms queued */
#define START_THRESHOLD_FRAMES (SAMPLE_RATE / 20)  /* 50 ms pre-roll after an underrun */

/* ---------- ring buffer (s16 interleaved stereo) ---------- */

static int16_t ring[RING_FRAMES * CHANNELS];
static int ring_read = 0, ring_write = 0, ring_fill = 0;   /* in frames */
static bool ring_primed = false;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile float volume_gain = 1.0f;

static void ring_clear(void) {
    pthread_mutex_lock(&ring_lock);
    ring_read = ring_write = ring_fill = 0;
    ring_primed = false;
    pthread_mutex_unlock(&ring_lock);
}

static void ring_put(const int16_t *pcm, int frames) {
    pthread_mutex_lock(&ring_lock);
    if (frames > RING_FRAMES) {
        pcm += (frames - RING_FRAMES) * CHANNELS;
        frames = RING_FRAMES;
    }
    int overflow = ring_fill + frames - MAX_LATENCY_FRAMES;
    if (overflow > 0) {
        /* too much queued: skip the oldest audio to keep latency bounded */
        if (overflow > ring_fill) overflow = ring_fill;
        ring_read = (ring_read + overflow) % RING_FRAMES;
        ring_fill -= overflow;
    }
    for (int done = 0; done < frames;) {
        int chunk = RING_FRAMES - ring_write;
        if (chunk > frames - done) chunk = frames - done;
        memcpy(&ring[ring_write * CHANNELS], &pcm[done * CHANNELS], (size_t) chunk * CHANNELS * sizeof(int16_t));
        ring_write = (ring_write + chunk) % RING_FRAMES;
        done += chunk;
    }
    ring_fill += frames;
    pthread_mutex_unlock(&ring_lock);
}

static void ring_get(int16_t *out, int frames) {
    pthread_mutex_lock(&ring_lock);
    if (!ring_primed && ring_fill >= START_THRESHOLD_FRAMES) ring_primed = true;
    int avail = ring_primed ? (ring_fill < frames ? ring_fill : frames) : 0;
    for (int done = 0; done < avail;) {
        int chunk = RING_FRAMES - ring_read;
        if (chunk > avail - done) chunk = avail - done;
        memcpy(&out[done * CHANNELS], &ring[ring_read * CHANNELS], (size_t) chunk * CHANNELS * sizeof(int16_t));
        ring_read = (ring_read + chunk) % RING_FRAMES;
        done += chunk;
    }
    ring_fill -= avail;
    if (avail < frames) {
        memset(&out[avail * CHANNELS], 0, (size_t) (frames - avail) * CHANNELS * sizeof(int16_t));
        if (ring_primed && ring_fill == 0) ring_primed = false;   /* underrun: re-buffer */
    }
    pthread_mutex_unlock(&ring_lock);

    float g = volume_gain;
    if (g < 0.999f) {
        for (int i = 0; i < frames * CHANNELS; i++) out[i] = (int16_t) ((float) out[i] * g);
    }
}

/* ---------- AAudio output ---------- */

static AAudioStream *stream = NULL;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool stream_needs_restart = false;

static aaudio_data_callback_result_t data_cb(AAudioStream *s, void *user, void *audio_data, int32_t num_frames) {
    (void) s;
    (void) user;
    ring_get((int16_t *) audio_data, num_frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void error_cb(AAudioStream *s, void *user, aaudio_result_t error) {
    (void) s;
    (void) user;
    LOGW("AAudio stream error %d (%s), will reopen", error, AAudio_convertResultToText(error));
    stream_needs_restart = true;   /* must not reopen from the callback thread */
}

static void close_stream_locked(void) {
    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
        stream = NULL;
    }
}

static bool open_stream_locked(void) {
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setChannelCount(builder, CHANNELS);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, data_cb, NULL);
    AAudioStreamBuilder_setErrorCallback(builder, error_cb, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK) {
        LOGE("AAudio open failed: %s", AAudio_convertResultToText(r));
        stream = NULL;
        return false;
    }
    r = AAudioStream_requestStart(stream);
    if (r != AAUDIO_OK) {
        LOGE("AAudio start failed: %s", AAudio_convertResultToText(r));
        AAudioStream_close(stream);
        stream = NULL;
        return false;
    }
    stream_needs_restart = false;
    LOGI("AAudio started: %d Hz, burst %d frames", AAudioStream_getSampleRate(stream),
         AAudioStream_getFramesPerBurst(stream));
    return true;
}

/* ---------- decoders ---------- */

static int current_ct = 0;
static AMediaCodec *aac = NULL;
static alac_wrapper_t *alac = NULL;

static void destroy_decoders_locked(void) {
    if (aac) {
        AMediaCodec_stop(aac);
        AMediaCodec_delete(aac);
        aac = NULL;
    }
    if (alac) {
        alac_wrapper_destroy(alac);
        alac = NULL;
    }
}

static bool create_aac_locked(bool eld) {
    static const uint8_t eld_config[] = {0xf8, 0xe8, 0x50, 0x00};   /* AAC-ELD 44100/2, 480 spf */
    static const uint8_t lc_config[] = {0x12, 0x10};                /* AAC-LC  44100/2 */
    aac = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    if (!aac) {
        LOGE("no AAC decoder");
        return false;
    }
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, SAMPLE_RATE);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, CHANNELS);
    AMediaFormat_setInt32(fmt, "aac-profile", eld ? 39 : 2);
    AMediaFormat_setInt32(fmt, "is-adts", 0);
    if (eld) {
        AMediaFormat_setBuffer(fmt, "csd-0", (void *) eld_config, sizeof(eld_config));
    } else {
        AMediaFormat_setBuffer(fmt, "csd-0", (void *) lc_config, sizeof(lc_config));
    }
    media_status_t st = AMediaCodec_configure(aac, fmt, NULL, NULL, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK || AMediaCodec_start(aac) != AMEDIA_OK) {
        LOGE("AAC decoder configure/start failed (%d)", st);
        AMediaCodec_delete(aac);
        aac = NULL;
        return false;
    }
    LOGI("AAC-%s decoder started", eld ? "ELD" : "LC");
    return true;
}

static void drain_aac_locked(void) {
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(aac, &info, 0);
        if (idx >= 0) {
            size_t size = 0;
            uint8_t *buf = AMediaCodec_getOutputBuffer(aac, (size_t) idx, &size);
            if (buf && info.size > 0) {
                ring_put((const int16_t *) (buf + info.offset), info.size / (int) (CHANNELS * sizeof(int16_t)));
            }
            AMediaCodec_releaseOutputBuffer(aac, (size_t) idx, false);
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *fmt = AMediaCodec_getOutputFormat(aac);
            int32_t rate = 0, ch = 0;
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
            AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
            LOGI("AAC output format: %d Hz, %d ch", rate, ch);
            AMediaFormat_delete(fmt);
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        } else {
            break;   /* try again later */
        }
    }
}

/* ---------- public API ---------- */

void audio_player_start(int ct) {
    pthread_mutex_lock(&state_lock);
    if (ct != current_ct || (!aac && !alac && ct != AUDIO_CT_PCM)) {
        destroy_decoders_locked();
        current_ct = ct;
        switch (ct) {
        case AUDIO_CT_AAC_ELD:
            create_aac_locked(true);
            break;
        case AUDIO_CT_AAC_LC:
            create_aac_locked(false);
            break;
        case AUDIO_CT_ALAC:
            alac = alac_wrapper_create();
            if (!alac) LOGE("ALAC decoder init failed");
            else LOGI("ALAC decoder started");
            break;
        case AUDIO_CT_PCM:
            LOGI("PCM audio");
            break;
        default:
            LOGW("unsupported audio compression type %d", ct);
            break;
        }
    }
    ring_clear();
    if (!stream) open_stream_locked();
    pthread_mutex_unlock(&state_lock);
}

void audio_player_push(const uint8_t *data, int len) {
    static int16_t pcm[4096 * CHANNELS];
    if (len <= 0) return;
    pthread_mutex_lock(&state_lock);
    if (stream_needs_restart || !stream) {
        close_stream_locked();
        open_stream_locked();
    }
    switch (current_ct) {
    case AUDIO_CT_AAC_ELD:
    case AUDIO_CT_AAC_LC:
        if (aac) {
            ssize_t idx = AMediaCodec_dequeueInputBuffer(aac, 10000);
            if (idx >= 0) {
                size_t cap = 0;
                uint8_t *buf = AMediaCodec_getInputBuffer(aac, (size_t) idx, &cap);
                size_t n = (size_t) len < cap ? (size_t) len : cap;
                if (buf) memcpy(buf, data, n);
                AMediaCodec_queueInputBuffer(aac, (size_t) idx, 0, buf ? n : 0, 0, 0);
            } else {
                LOGW("AAC input full, dropping packet");
            }
            drain_aac_locked();
        }
        break;
    case AUDIO_CT_ALAC:
        if (alac) {
            int frames = alac_wrapper_decode(alac, data, len, pcm, 4096);
            if (frames > 0) ring_put(pcm, frames);
        }
        break;
    case AUDIO_CT_PCM:
        ring_put((const int16_t *) data, len / (int) (CHANNELS * sizeof(int16_t)));
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&state_lock);
}

void audio_player_flush(void) {
    pthread_mutex_lock(&state_lock);
    if (aac) AMediaCodec_flush(aac);
    pthread_mutex_unlock(&state_lock);
    ring_clear();
}

void audio_player_set_volume(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    volume_gain = gain;
}

void audio_player_stop(void) {
    pthread_mutex_lock(&state_lock);
    close_stream_locked();
    destroy_decoders_locked();
    current_ct = 0;
    pthread_mutex_unlock(&state_lock);
    ring_clear();
}
