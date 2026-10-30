/*
 * MirrorTV - JNI glue: runs UxPlay's AirPlay server (raop) inside an Android
 * app, routes video to the hardware decoder, audio to AAudio and service
 * discovery to Java's NsdManager.
 *
 * License: GPL-3.0-or-later
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"

#include "audio_player.h"
#include "mdns_bridge.h"
#include "video_decoder.h"

#define TAG "MirrorTV"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define BRIDGE_CLASS "hu/balazs/mirrortv/NativeBridge"
#define DEFAULT_PORT 7000
#define MISSED_FEEDBACK_LIMIT 15   /* seconds without client heartbeat before reset */

/* ---------- JNI plumbing ---------- */

static JavaVM *jvm = NULL;
static jclass bridge_class = NULL;
static jmethodID m_on_mdns_register, m_on_mdns_unregister, m_on_video_size, m_on_first_frame,
                 m_on_client, m_on_connections, m_on_mirror_stopped;
static pthread_key_t detach_key;

static void detach_thread(void *unused) {
    (void) unused;
    if (jvm) (*jvm)->DetachCurrentThread(jvm);
}

static JNIEnv *get_env(void) {
    JNIEnv *env = NULL;
    if (!jvm) return NULL;
    jint r = (*jvm)->GetEnv(jvm, (void **) &env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
#ifdef __ANDROID__
        if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK) return NULL;
#else
        if ((*jvm)->AttachCurrentThread(jvm, (void **) &env, NULL) != JNI_OK) return NULL;
#endif
        pthread_setspecific(detach_key, (void *) 1);   /* detach when this native thread exits */
    } else if (r != JNI_OK) {
        return NULL;
    }
    return env;
}

static void clear_exception(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

void mdns_bridge_register(int service, const char *name, unsigned short port,
                          int count, const char **keys, const char **values) {
    JNIEnv *env = get_env();
    if (!env) return;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jobjectArray jkeys = (*env)->NewObjectArray(env, count, string_class, NULL);
    jobjectArray jvalues = (*env)->NewObjectArray(env, count, string_class, NULL);
    for (int i = 0; i < count; i++) {
        jstring k = (*env)->NewStringUTF(env, keys[i]);
        jstring v = (*env)->NewStringUTF(env, values[i]);
        (*env)->SetObjectArrayElement(env, jkeys, i, k);
        (*env)->SetObjectArrayElement(env, jvalues, i, v);
        (*env)->DeleteLocalRef(env, k);
        (*env)->DeleteLocalRef(env, v);
    }
    jstring jname = (*env)->NewStringUTF(env, name);
    (*env)->CallStaticVoidMethod(env, bridge_class, m_on_mdns_register, service, jname, (jint) port, jkeys, jvalues);
    clear_exception(env);
    (*env)->DeleteLocalRef(env, jname);
    (*env)->DeleteLocalRef(env, jkeys);
    (*env)->DeleteLocalRef(env, jvalues);
    (*env)->DeleteLocalRef(env, string_class);
}

void mdns_bridge_unregister(int service) {
    JNIEnv *env = get_env();
    if (!env) return;
    (*env)->CallStaticVoidMethod(env, bridge_class, m_on_mdns_unregister, service);
    clear_exception(env);
}

static void notify_video_size(int w, int h) {
    JNIEnv *env = get_env();
    if (!env) return;
    (*env)->CallStaticVoidMethod(env, bridge_class, m_on_video_size, w, h);
    clear_exception(env);
}

static void notify_first_frame(void) {
    JNIEnv *env = get_env();
    if (!env) return;
    (*env)->CallStaticVoidMethod(env, bridge_class, m_on_first_frame);
    clear_exception(env);
}

static void notify_client(const char *name, const char *model) {
    JNIEnv *env = get_env();
    if (!env) return;
    jstring jn = (*env)->NewStringUTF(env, name ? name : "");
    jstring jm = (*env)->NewStringUTF(env, model ? model : "");
    (*env)->CallStaticVoidMethod(env, bridge_class, m_on_client, jn, jm);
    clear_exception(env);
    (*env)->DeleteLocalRef(env, jn);
    (*env)->DeleteLocalRef(env, jm);
}

static void notify_connections(int count) {
    JNIEnv *env = get_env();
    if (!env) return;
    (*env)->CallStaticVoidMethod(env, bridge_class, m_on_connections, count);
    clear_exception(env);
}

static void notify_mirror_stopped(void) {
    JNIEnv *env = get_env();
    if (!env) return;
    (*env)->CallStaticVoidMethod(env, bridge_class, m_on_mirror_stopped);
    clear_exception(env);
}

/* ---------- server state ---------- */

static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;
static raop_t *raop = NULL;
static dnssd_t *dnssd = NULL;
static unsigned short raop_port = 0;

static volatile int open_connections = 0;
static volatile int missed_feedback = 0;
static uint64_t remote_clock_offset = 0;

/* supervisor thread: performs httpd resets requested from raop callbacks
 * (which cannot do it themselves because they run on raop's own threads) */
static pthread_t supervisor;
static bool supervisor_running = false;
static pthread_mutex_t sup_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sup_cond = PTHREAD_COND_INITIALIZER;
static bool sup_stop = false;
static bool sup_reset_httpd = false;
static bool sup_reset_video = false;

static void request_reset(bool httpd) {
    pthread_mutex_lock(&sup_lock);
    if (httpd) sup_reset_httpd = true;
    sup_reset_video = true;
    pthread_cond_signal(&sup_cond);
    pthread_mutex_unlock(&sup_lock);
}

static void *supervisor_loop(void *arg) {
    (void) arg;
    pthread_mutex_lock(&sup_lock);
    while (!sup_stop) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&sup_cond, &sup_lock, &ts);
        if (sup_stop) break;

        /* client heartbeat watchdog (the iPhone sends feedback every ~2 s) */
        if (open_connections > 0) {
            missed_feedback++;
            if (missed_feedback > MISSED_FEEDBACK_LIMIT) {
                LOGW("no feedback from client for %d s: resetting connection", missed_feedback);
                sup_reset_httpd = true;
                sup_reset_video = true;
                missed_feedback = 0;
            }
        } else {
            missed_feedback = 0;
        }

        if (sup_reset_video || sup_reset_httpd) {
            bool reset_httpd = sup_reset_httpd;
            sup_reset_httpd = sup_reset_video = false;
            pthread_mutex_unlock(&sup_lock);

            pthread_mutex_lock(&server_lock);
            if (raop) {
                if (reset_httpd) raop_stop_httpd(raop);
                audio_player_stop();
                video_decoder_reset();
                remote_clock_offset = 0;
                if (reset_httpd) {
                    raop_remove_known_connections(raop);
                    unsigned short port = raop_get_port(raop);
                    raop_start_httpd(raop, &port);
                    raop_set_port(raop, port);
                    LOGI("AirPlay server restarted on port %u", port);
                }
            }
            pthread_mutex_unlock(&server_lock);
            notify_mirror_stopped();

            pthread_mutex_lock(&sup_lock);
        }
    }
    pthread_mutex_unlock(&sup_lock);
    return NULL;
}

/* ---------- raop callbacks ---------- */

static void cb_conn_init(void *cls) {
    (void) cls;
    open_connections++;
    missed_feedback = 0;
    LOGI("open connections: %d", open_connections);
    notify_connections(open_connections);
}

static void cb_conn_destroy(void *cls) {
    (void) cls;
    if (open_connections > 0) open_connections--;
    LOGI("open connections: %d", open_connections);
    if (open_connections == 0) {
        remote_clock_offset = 0;
        audio_player_stop();
    }
    notify_connections(open_connections);
}

static void cb_conn_feedback(void *cls) {
    (void) cls;
    missed_feedback = 0;
}

static void cb_conn_reset(void *cls, int reason) {
    (void) cls;
    LOGI("connection reset requested (reason %d)", reason);
    request_reset(true);
}

static void cb_video_reset(void *cls, reset_type_t type) {
    (void) cls;
    LOGI("video reset, type %d", (int) type);
    switch (type) {
    case RESET_TYPE_NOHOLD:
    case RESET_TYPE_HLS_EOS:
    case RESET_TYPE_RTP_SHUTDOWN:
    case RESET_TYPE_RTP_TO_HLS_TEARDOWN:
    case RESET_TYPE_HLS_SHUTDOWN:
        /* mirroring ended or was taken over: drop the decoder, keep the server */
        video_decoder_reset();
        remote_clock_offset = 0;
        notify_mirror_stopped();
        break;
    default:
        break;
    }
}

static int cb_video_set_codec_and_size(void *cls, video_codec_t codec, unsigned short width_source,
                                       unsigned short height_source, unsigned short x_offset,
                                       unsigned short y_offset, unsigned short width, unsigned short height) {
    (void) cls;
    (void) x_offset;
    (void) y_offset;
    LOGI("video codec %s, source %ux%u, visible %ux%u", codec == VIDEO_CODEC_H265 ? "H.265" : "H.264",
         width_source, height_source, width, height);
    if (codec == VIDEO_CODEC_H265) return -1;   /* not advertised; refuse */
    return 0;
}

static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    (void) cls;
    (void) ntp;
    if (data->is_h265) return;
    uint64_t pts_ns = data->ntp_time_remote;
    video_decoder_push(data->data, data->data_len, pts_ns / 1000);
}

static void cb_video_flush(void *cls) {
    (void) cls;
    video_decoder_flush();
}

static void cb_video_pause(void *cls) { (void) cls; }
static void cb_video_resume(void *cls) { (void) cls; }

static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *using_screen,
                                bool *is_media, uint64_t *audio_format) {
    (void) cls;
    LOGI("audio format: ct=%d spf=%d usingScreen=%d isMedia=%d format=0x%llx", *ct, *spf, *using_screen,
         *is_media, (unsigned long long) *audio_format);
    audio_player_start(*ct);
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    (void) cls;
    (void) ntp;
    audio_player_push(data->data, data->data_len);
}

static void cb_audio_flush(void *cls) {
    (void) cls;
    audio_player_flush();
}

static float current_volume_db = 0.0f;

static double cb_audio_set_client_volume(void *cls) {
    (void) cls;
    return (double) current_volume_db;
}

static void cb_audio_set_volume(void *cls, float volume) {
    (void) cls;
    /* AirPlay volume: -30 dB .. 0 dB, -144 = mute */
    float gain;
    if (volume <= -30.0f) {
        gain = 0.0f;
    } else if (volume >= 0.0f) {
        gain = 1.0f;
    } else {
        float frac = (30.0f + volume) / 30.0f;   /* slider position 0..1 */
        gain = frac * frac;                     /* roughly perceptual */
    }
    current_volume_db = volume < -30.0f ? -30.0f : volume;
    audio_player_set_volume(gain);
    LOGI("volume %.1f dB -> gain %.3f", volume, gain);
}

static void cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit) {
    (void) cls;
    LOGI("connection request from %s (%s), deviceID %s", name, model, deviceid);
    *admit = true;
    notify_client(name, model);
}

static void cb_log(void *cls, int level, const char *msg) {
    (void) cls;
    int prio;
    switch (level) {
    case LOGGER_EMERG:
    case LOGGER_ALERT:
    case LOGGER_CRIT:
    case LOGGER_ERR:
        prio = ANDROID_LOG_ERROR;
        break;
    case LOGGER_WARNING:
        prio = ANDROID_LOG_WARN;
        break;
    case LOGGER_NOTICE:
    case LOGGER_INFO:
        prio = ANDROID_LOG_INFO;
        break;
    default:
        prio = ANDROID_LOG_DEBUG;
        break;
    }
    __android_log_print(prio, "MirrorTV-raop", "%s", msg);
}

/* ---------- JNI entry points ---------- */

static void stop_server_locked(void) {
    if (supervisor_running) {
        pthread_mutex_lock(&sup_lock);
        sup_stop = true;
        pthread_cond_signal(&sup_cond);
        pthread_mutex_unlock(&sup_lock);
        pthread_mutex_unlock(&server_lock);   /* supervisor may need server_lock to finish */
        pthread_join(supervisor, NULL);
        pthread_mutex_lock(&server_lock);
        supervisor_running = false;
    }
    if (dnssd) {
        dnssd_unregister_raop(dnssd);
        dnssd_unregister_airplay(dnssd);
    }
    if (raop) {
        raop_destroy(raop);
        raop = NULL;
    }
    if (dnssd) {
        dnssd_destroy(dnssd);
        dnssd = NULL;
    }
    audio_player_stop();
    video_decoder_reset();
    open_connections = 0;
}

JNIEXPORT jint JNICALL
Java_hu_balazs_mirrortv_NativeBridge_nativeStart(JNIEnv *env, jclass clazz, jstring jname, jbyteArray jhwaddr,
                                                 jstring jkeyfile, jint width, jint height, jint fps,
                                                 jboolean debug) {
    (void) clazz;
    int result = 0;
    pthread_mutex_lock(&server_lock);
    if (raop) {
        pthread_mutex_unlock(&server_lock);
        return raop_port;
    }

    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
    const char *keyfile = (*env)->GetStringUTFChars(env, jkeyfile, NULL);
    jsize hw_len = (*env)->GetArrayLength(env, jhwaddr);
    jbyte hw[6] = {0};
    (*env)->GetByteArrayRegion(env, jhwaddr, 0, hw_len < 6 ? hw_len : 6, hw);

    char device_id[18];
    snprintf(device_id, sizeof(device_id), "%02x:%02x:%02x:%02x:%02x:%02x", (uint8_t) hw[0], (uint8_t) hw[1],
             (uint8_t) hw[2], (uint8_t) hw[3], (uint8_t) hw[4], (uint8_t) hw[5]);

    int err = 0;
    dnssd = dnssd_init(name, (int) strlen(name), (const char *) hw, 6, 0, &err);
    if (err || !dnssd) {
        LOGE("dnssd_init failed: %d", err);
        result = -1;
        goto out;
    }
    dnssd_set_airplay_features(dnssd, 4, 0);    /* no HLS video (YouTube "AirPlay video" falls back to mirroring) */
    dnssd_set_airplay_features(dnssd, 42, 0);   /* H.264 only */
    dnssd_set_airplay_features(dnssd, 27, 1);   /* legacy pairing */

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.conn_init = cb_conn_init;
    cbs.conn_destroy = cb_conn_destroy;
    cbs.conn_reset = cb_conn_reset;
    cbs.conn_feedback = cb_conn_feedback;
    cbs.audio_process = cb_audio_process;
    cbs.video_process = cb_video_process;
    cbs.audio_flush = cb_audio_flush;
    cbs.video_flush = cb_video_flush;
    cbs.video_pause = cb_video_pause;
    cbs.video_resume = cb_video_resume;
    cbs.audio_set_client_volume = cb_audio_set_client_volume;
    cbs.audio_set_volume = cb_audio_set_volume;
    cbs.audio_get_format = cb_audio_get_format;
    cbs.report_client_request = cb_report_client_request;
    cbs.video_reset = cb_video_reset;
    cbs.video_set_codec_and_size = cb_video_set_codec_and_size;

    raop = raop_init(&cbs);
    if (!raop) {
        LOGE("raop_init failed");
        result = -2;
        goto out;
    }
    raop_set_log_callback(raop, cb_log, NULL);
    raop_set_log_level(raop, debug ? LOGGER_DEBUG : LOGGER_INFO);
    if (raop_init2(raop, 1, device_id, keyfile)) {
        LOGE("raop_init2 failed");
        free(raop);
        raop = NULL;
        result = -3;
        goto out;
    }
    if (width > 0) raop_set_plist(raop, "width", width);
    if (height > 0) raop_set_plist(raop, "height", height);
    if (fps > 0) {
        raop_set_plist(raop, "refreshRate", fps);
        raop_set_plist(raop, "maxFPS", fps);
    }

    unsigned short tcp[3] = {0, DEFAULT_PORT, 0};
    unsigned short udp[3] = {0, 0, 0};
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);

    raop_port = raop_get_port(raop);
    if (raop_start_httpd(raop, &raop_port) < 0) {
        LOGW("port %d busy, using a random port", DEFAULT_PORT);
        raop_port = 0;
        if (raop_start_httpd(raop, &raop_port) < 0) {
            LOGE("could not start AirPlay HTTP server");
            raop_destroy(raop);
            raop = NULL;
            result = -4;
            goto out;
        }
    }
    raop_set_port(raop, raop_port);
    raop_set_dnssd(raop, dnssd);

    if (dnssd_register_raop(dnssd, raop_port) || dnssd_register_airplay(dnssd, raop_port)) {
        LOGE("service registration failed");
        result = -5;
        goto out;
    }

    sup_stop = false;
    sup_reset_httpd = sup_reset_video = false;
    if (pthread_create(&supervisor, NULL, supervisor_loop, NULL) == 0) supervisor_running = true;

    LOGI("AirPlay receiver \"%s\" running on port %u (deviceID %s)", name, raop_port, device_id);
    result = raop_port;

out:
    if (result < 0) {
        if (raop) {
            raop_destroy(raop);
            raop = NULL;
        }
        if (dnssd) {
            dnssd_destroy(dnssd);
            dnssd = NULL;
        }
    }
    (*env)->ReleaseStringUTFChars(env, jname, name);
    (*env)->ReleaseStringUTFChars(env, jkeyfile, keyfile);
    pthread_mutex_unlock(&server_lock);
    return result;
}

JNIEXPORT void JNICALL
Java_hu_balazs_mirrortv_NativeBridge_nativeStop(JNIEnv *env, jclass clazz) {
    (void) env;
    (void) clazz;
    pthread_mutex_lock(&server_lock);
    stop_server_locked();
    pthread_mutex_unlock(&server_lock);
    LOGI("AirPlay receiver stopped");
}

JNIEXPORT void JNICALL
Java_hu_balazs_mirrortv_NativeBridge_nativeSetSurface(JNIEnv *env, jclass clazz, jobject surface) {
    (void) clazz;
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    video_decoder_set_window(window);
}

JNIEXPORT void JNICALL
Java_hu_balazs_mirrortv_NativeBridge_nativeDisconnect(JNIEnv *env, jclass clazz) {
    (void) env;
    (void) clazz;
    request_reset(true);
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void) reserved;
    JNIEnv *env = NULL;
    jvm = vm;
    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    pthread_key_create(&detach_key, detach_thread);

    jclass local = (*env)->FindClass(env, BRIDGE_CLASS);
    if (!local) return JNI_ERR;
    bridge_class = (jclass) (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);

    m_on_mdns_register = (*env)->GetStaticMethodID(env, bridge_class, "onMdnsRegister",
                                                   "(ILjava/lang/String;I[Ljava/lang/String;[Ljava/lang/String;)V");
    m_on_mdns_unregister = (*env)->GetStaticMethodID(env, bridge_class, "onMdnsUnregister", "(I)V");
    m_on_video_size = (*env)->GetStaticMethodID(env, bridge_class, "onVideoSize", "(II)V");
    m_on_first_frame = (*env)->GetStaticMethodID(env, bridge_class, "onFirstFrame", "()V");
    m_on_client = (*env)->GetStaticMethodID(env, bridge_class, "onClient", "(Ljava/lang/String;Ljava/lang/String;)V");
    m_on_connections = (*env)->GetStaticMethodID(env, bridge_class, "onConnections", "(I)V");
    m_on_mirror_stopped = (*env)->GetStaticMethodID(env, bridge_class, "onMirrorStopped", "()V");
    if (!m_on_mdns_register || !m_on_mdns_unregister || !m_on_video_size || !m_on_first_frame ||
        !m_on_client || !m_on_connections || !m_on_mirror_stopped) {
        return JNI_ERR;
    }

    ntp_global_init();
    video_decoder_init(notify_video_size, notify_first_frame);
    return JNI_VERSION_1_6;
}
