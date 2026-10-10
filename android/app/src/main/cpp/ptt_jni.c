#include <jni.h>

#include <android/log.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ptt_audio.h"
#include "ptt_client.h"

#define LOG_TAG "SipPttNative"
#define PLAYBACK_QUEUE_SAMPLES (PTT_AUDIO_SAMPLE_RATE * 2u)

typedef struct {
    pthread_mutex_t mutex;
    pthread_mutex_t audio_mutex;
    pthread_t thread;
    int thread_started;
    int running;
    ptt_client *client;
    ptt_audio_encoder *encoder;
    ptt_audio_decoder *decoder;
    int16_t playback_queue[PLAYBACK_QUEUE_SAMPLES];
    size_t playback_read;
    size_t playback_write;
    size_t playback_count;
    uint64_t tx_stats_started_ms;
    uint64_t tx_stats_input_frames;
    uint64_t tx_stats_input_samples;
    uint64_t tx_stats_encoded_packets;
    uint64_t tx_stats_encoded_bytes;
    size_t tx_stats_encoded_min_bytes;
    size_t tx_stats_encoded_max_bytes;
    uint64_t tx_stats_encode_errors;
    uint64_t tx_stats_send_errors;
    uint64_t tx_stats_not_ready;
} android_ptt_context;

static android_ptt_context g_context = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .audio_mutex = PTHREAD_MUTEX_INITIALIZER,
    .thread_started = 0,
    .running = 0,
    .client = NULL,
    .encoder = NULL,
    .decoder = NULL
};

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void reset_tx_stats_locked(uint64_t now_ms)
{
    g_context.tx_stats_started_ms = now_ms;
    g_context.tx_stats_input_frames = 0;
    g_context.tx_stats_input_samples = 0;
    g_context.tx_stats_encoded_packets = 0;
    g_context.tx_stats_encoded_bytes = 0;
    g_context.tx_stats_encoded_min_bytes = 0;
    g_context.tx_stats_encoded_max_bytes = 0;
    g_context.tx_stats_encode_errors = 0;
    g_context.tx_stats_send_errors = 0;
    g_context.tx_stats_not_ready = 0;
}

static void maybe_log_tx_stats_locked(uint64_t now_ms)
{
    double average_payload = 0.0;

    if (g_context.tx_stats_started_ms == 0) {
        reset_tx_stats_locked(now_ms);
        return;
    }
    if (now_ms - g_context.tx_stats_started_ms < 1000u) {
        return;
    }
    if (g_context.tx_stats_encoded_packets > 0) {
        average_payload =
            (double)g_context.tx_stats_encoded_bytes /
            (double)g_context.tx_stats_encoded_packets;
    }
    __android_log_print(
        ANDROID_LOG_INFO,
        LOG_TAG,
        "[audio-tx/1s] input_frames=%llu input_samples=%llu "
        "encoded_packets=%llu encoded_bytes=%llu payload_bytes avg=%.1f min=%zu max=%zu "
        "encode_errors=%llu send_errors=%llu not_ready=%llu",
        (unsigned long long)g_context.tx_stats_input_frames,
        (unsigned long long)g_context.tx_stats_input_samples,
        (unsigned long long)g_context.tx_stats_encoded_packets,
        (unsigned long long)g_context.tx_stats_encoded_bytes,
        average_payload,
        g_context.tx_stats_encoded_min_bytes,
        g_context.tx_stats_encoded_max_bytes,
        (unsigned long long)g_context.tx_stats_encode_errors,
        (unsigned long long)g_context.tx_stats_send_errors,
        (unsigned long long)g_context.tx_stats_not_ready);
    reset_tx_stats_locked(now_ms);
}

static void android_log(void *user_data, const char *message)
{
    (void)user_data;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", message);
}

static void enqueue_playback_locked(const int16_t *pcm, size_t sample_count)
{
    size_t index;

    if (sample_count > PLAYBACK_QUEUE_SAMPLES) {
        pcm += sample_count - PLAYBACK_QUEUE_SAMPLES;
        sample_count = PLAYBACK_QUEUE_SAMPLES;
    }
    while (g_context.playback_count + sample_count > PLAYBACK_QUEUE_SAMPLES) {
        g_context.playback_read =
            (g_context.playback_read + 1u) % PLAYBACK_QUEUE_SAMPLES;
        --g_context.playback_count;
    }
    for (index = 0; index < sample_count; ++index) {
        g_context.playback_queue[g_context.playback_write] = pcm[index];
        g_context.playback_write =
            (g_context.playback_write + 1u) % PLAYBACK_QUEUE_SAMPLES;
    }
    g_context.playback_count += sample_count;
}

static void receive_opus(void *user_data,
                         const uint8_t *payload,
                         size_t payload_length,
                         uint16_t sequence)
{
    int16_t decoded[PTT_AUDIO_MAX_DECODE_SAMPLES];
    int samples;

    (void)user_data;
    (void)sequence;
    pthread_mutex_lock(&g_context.audio_mutex);
    if (g_context.decoder == NULL) {
        pthread_mutex_unlock(&g_context.audio_mutex);
        return;
    }
    samples = ptt_audio_decode_opus(g_context.decoder,
                                    payload,
                                    payload_length,
                                    decoded,
                                    PTT_AUDIO_MAX_DECODE_SAMPLES);
    if (samples > 0) {
        enqueue_playback_locked(decoded, (size_t)samples);
    }
    pthread_mutex_unlock(&g_context.audio_mutex);
}

static void *network_thread(void *argument)
{
    int keep_running;
    int result;

    (void)argument;
    pthread_mutex_lock(&g_context.mutex);
    if (!g_context.running || g_context.client == NULL) {
        pthread_mutex_unlock(&g_context.mutex);
        return NULL;
    }
    result = ptt_client_start(g_context.client);
    if (result != 0) {
        g_context.running = 0;
    }
    pthread_mutex_unlock(&g_context.mutex);

    for (;;) {
        pthread_mutex_lock(&g_context.mutex);
        keep_running = g_context.running && g_context.client != NULL;
        if (keep_running) {
            result = ptt_client_poll(g_context.client, 20);
            if (result != 0) {
                g_context.running = 0;
                keep_running = 0;
            }
        }
        pthread_mutex_unlock(&g_context.mutex);

        if (!keep_running) {
            break;
        }
    }
    return NULL;
}

static void shutdown_context(void)
{
    pthread_t thread;
    int should_join;
    ptt_audio_encoder *encoder;
    ptt_audio_decoder *decoder;

    pthread_mutex_lock(&g_context.mutex);
    g_context.running = 0;
    should_join = g_context.thread_started;
    thread = g_context.thread;
    pthread_mutex_unlock(&g_context.mutex);

    if (should_join && !pthread_equal(pthread_self(), thread)) {
        (void)pthread_join(thread, NULL);
    }

    pthread_mutex_lock(&g_context.mutex);
    g_context.thread_started = 0;
    if (g_context.client != NULL) {
        ptt_client_destroy(g_context.client);
        g_context.client = NULL;
    }
    encoder = g_context.encoder;
    g_context.encoder = NULL;
    reset_tx_stats_locked(0);
    pthread_mutex_unlock(&g_context.mutex);

    pthread_mutex_lock(&g_context.audio_mutex);
    decoder = g_context.decoder;
    g_context.decoder = NULL;
    g_context.playback_read = 0;
    g_context.playback_write = 0;
    g_context.playback_count = 0;
    pthread_mutex_unlock(&g_context.audio_mutex);

    ptt_audio_encoder_destroy(encoder);
    ptt_audio_decoder_destroy(decoder);
}

JNIEXPORT jboolean JNICALL
Java_com_master_sipptt_NativePtt_nativeStart(JNIEnv *env,
                                              jclass clazz,
                                              jstring server,
                                              jstring device_id,
                                              jstring group_id)
{
    const char *server_text;
    const char *device_text;
    const char *group_text;
    ptt_client_config config;
    ptt_client *client;
    ptt_audio_encoder *encoder;
    ptt_audio_decoder *decoder;
    int thread_result;

    (void)clazz;
    if (server == NULL || device_id == NULL || group_id == NULL) {
        return JNI_FALSE;
    }

    server_text = (*env)->GetStringUTFChars(env, server, NULL);
    device_text = (*env)->GetStringUTFChars(env, device_id, NULL);
    group_text = (*env)->GetStringUTFChars(env, group_id, NULL);
    if (server_text == NULL || device_text == NULL || group_text == NULL) {
        if (server_text != NULL) (*env)->ReleaseStringUTFChars(env, server, server_text);
        if (device_text != NULL) (*env)->ReleaseStringUTFChars(env, device_id, device_text);
        if (group_text != NULL) (*env)->ReleaseStringUTFChars(env, group_id, group_text);
        return JNI_FALSE;
    }

    shutdown_context();
    encoder = ptt_audio_encoder_create(16000u);
    decoder = ptt_audio_decoder_create();
    ptt_client_config_init(&config);
    snprintf(config.server_host, sizeof(config.server_host), "%s", server_text);
    snprintf(config.device_id, sizeof(config.device_id), "%s", device_text);
    snprintf(config.group_id, sizeof(config.group_id), "%s", group_text);
    config.local_sip_port = 0;
    config.local_rtp_port = 0;
    config.simulate_opus = 0;
    config.log_fn = android_log;
    config.log_user_data = NULL;
    config.opus_receive_fn = receive_opus;
    config.opus_receive_user_data = NULL;

    (*env)->ReleaseStringUTFChars(env, server, server_text);
    (*env)->ReleaseStringUTFChars(env, device_id, device_text);
    (*env)->ReleaseStringUTFChars(env, group_id, group_text);

    if (encoder == NULL || decoder == NULL) {
        ptt_audio_encoder_destroy(encoder);
        ptt_audio_decoder_destroy(decoder);
        return JNI_FALSE;
    }
    client = ptt_client_create(&config);
    if (client == NULL) {
        ptt_audio_encoder_destroy(encoder);
        ptt_audio_decoder_destroy(decoder);
        return JNI_FALSE;
    }

    pthread_mutex_lock(&g_context.mutex);
    g_context.client = client;
    g_context.encoder = encoder;
    reset_tx_stats_locked(monotonic_ms());
    pthread_mutex_lock(&g_context.audio_mutex);
    g_context.decoder = decoder;
    g_context.playback_read = 0;
    g_context.playback_write = 0;
    g_context.playback_count = 0;
    pthread_mutex_unlock(&g_context.audio_mutex);
    g_context.running = 1;
    thread_result = pthread_create(&g_context.thread, NULL, network_thread, NULL);
    if (thread_result == 0) {
        g_context.thread_started = 1;
    } else {
        g_context.running = 0;
    }
    pthread_mutex_unlock(&g_context.mutex);

    if (thread_result != 0) {
        shutdown_context();
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_master_sipptt_NativePtt_nativeRequestTalk(JNIEnv *env, jclass clazz)
{
    int result = -1;

    (void)env;
    (void)clazz;
    pthread_mutex_lock(&g_context.mutex);
    if (g_context.client != NULL) {
        result = ptt_client_request_talk(g_context.client);
    }
    pthread_mutex_unlock(&g_context.mutex);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_master_sipptt_NativePtt_nativeReleaseTalk(JNIEnv *env, jclass clazz)
{
    int result = -1;

    (void)env;
    (void)clazz;
    pthread_mutex_lock(&g_context.mutex);
    if (g_context.client != NULL) {
        result = ptt_client_release_talk(g_context.client);
    }
    pthread_mutex_unlock(&g_context.mutex);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_master_sipptt_NativePtt_nativePushCapturePcm(JNIEnv *env,
                                                       jclass clazz,
                                                       jshortArray pcm,
                                                       jint samples)
{
    int16_t capture[PTT_CAPTURE_FRAME_SAMPLES];
    uint8_t packet[PTT_OPUS_MAX_PACKET_BYTES];
    int encoded = -1;
    int result = -1;
    uint64_t now_ms;

    (void)clazz;
    if (pcm == NULL || samples != (jint)PTT_CAPTURE_FRAME_SAMPLES ||
        (*env)->GetArrayLength(env, pcm) < samples) {
        return JNI_FALSE;
    }
    (*env)->GetShortArrayRegion(env, pcm, 0, samples, (jshort *)capture);
    if ((*env)->ExceptionCheck(env)) {
        return JNI_FALSE;
    }

    now_ms = monotonic_ms();
    pthread_mutex_lock(&g_context.mutex);
    ++g_context.tx_stats_input_frames;
    g_context.tx_stats_input_samples += (uint64_t)samples;
    if (g_context.running && g_context.client != NULL && g_context.encoder != NULL) {
        encoded = ptt_audio_encode_pcm48(g_context.encoder,
                                         capture,
                                         PTT_CAPTURE_FRAME_SAMPLES,
                                         packet,
                                         sizeof(packet));
        if (encoded > 0) {
            ++g_context.tx_stats_encoded_packets;
            g_context.tx_stats_encoded_bytes += (uint64_t)encoded;
            if (g_context.tx_stats_encoded_min_bytes == 0 ||
                (size_t)encoded < g_context.tx_stats_encoded_min_bytes) {
                g_context.tx_stats_encoded_min_bytes = (size_t)encoded;
            }
            if ((size_t)encoded > g_context.tx_stats_encoded_max_bytes) {
                g_context.tx_stats_encoded_max_bytes = (size_t)encoded;
            }
            result = ptt_client_send_opus(g_context.client,
                                          packet,
                                          (size_t)encoded);
            if (result != 0) {
                ++g_context.tx_stats_send_errors;
            }
        } else {
            ++g_context.tx_stats_encode_errors;
        }
    } else {
        ++g_context.tx_stats_not_ready;
    }
    maybe_log_tx_stats_locked(now_ms);
    pthread_mutex_unlock(&g_context.mutex);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_master_sipptt_NativePtt_nativeReadPlaybackPcm(JNIEnv *env,
                                                        jclass clazz,
                                                        jshortArray pcm,
                                                        jint max_samples)
{
    int16_t output[PTT_AUDIO_MAX_DECODE_SAMPLES];
    size_t requested;
    size_t count;
    size_t index;

    (void)clazz;
    if (pcm == NULL || max_samples <= 0) {
        return 0;
    }
    requested = (size_t)max_samples;
    if (requested > (size_t)(*env)->GetArrayLength(env, pcm)) {
        requested = (size_t)(*env)->GetArrayLength(env, pcm);
    }
    if (requested > PTT_AUDIO_MAX_DECODE_SAMPLES) {
        requested = PTT_AUDIO_MAX_DECODE_SAMPLES;
    }

    pthread_mutex_lock(&g_context.audio_mutex);
    count = g_context.playback_count < requested ?
        g_context.playback_count : requested;
    for (index = 0; index < count; ++index) {
        output[index] = g_context.playback_queue[g_context.playback_read];
        g_context.playback_read =
            (g_context.playback_read + 1u) % PLAYBACK_QUEUE_SAMPLES;
    }
    g_context.playback_count -= count;
    pthread_mutex_unlock(&g_context.audio_mutex);

    if (count > 0) {
        (*env)->SetShortArrayRegion(env, pcm, 0, (jsize)count, (const jshort *)output);
        if ((*env)->ExceptionCheck(env)) {
            return 0;
        }
    }
    return (jint)count;
}

JNIEXPORT jstring JNICALL
Java_com_master_sipptt_NativePtt_nativeGetStatus(JNIEnv *env, jclass clazz)
{
    ptt_client_snapshot snapshot;
    char status[768];

    (void)clazz;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.mode = PTT_CLIENT_STOPPED;
    snprintf(snapshot.detail, sizeof(snapshot.detail), "%s", "尚未加入组队");

    pthread_mutex_lock(&g_context.mutex);
    if (g_context.client != NULL) {
        ptt_client_get_snapshot(g_context.client, &snapshot);
    }
    pthread_mutex_unlock(&g_context.mutex);

    snprintf(status,
             sizeof(status),
             "%s\t%d\t%d\t%d\t%u\t%u\t%llu\t%llu\t%llu\t%.1f\t%lld\t%lld\t%s\t%s",
             ptt_client_mode_name(snapshot.mode),
             snapshot.registered,
             snapshot.joined,
             snapshot.floor_granted,
             snapshot.local_sip_port,
             snapshot.local_rtp_port,
             (unsigned long long)snapshot.transmitted_packets,
             (unsigned long long)snapshot.received_packets,
             (unsigned long long)snapshot.last_rx_packets_per_second,
             snapshot.last_latency_average_ms,
             (long long)snapshot.last_latency_min_ms,
             (long long)snapshot.last_latency_max_ms,
             snapshot.detail,
             snapshot.speaker);
    return (*env)->NewStringUTF(env, status);
}

JNIEXPORT void JNICALL
Java_com_master_sipptt_NativePtt_nativeShutdown(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    shutdown_context();
}
