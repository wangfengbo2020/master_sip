#include <jni.h>

#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ptt_audio.h"
#include "ptt_client.h"

#define LOG_TAG "SipPttNative"
#define PLAYBACK_QUEUE_FRAMES PTT_AUDIO_QUEUE_HARD_LIMIT_FRAMES
#define PLAYBACK_QUEUE_RETAIN_FRAMES PTT_AUDIO_QUEUE_RETAIN_FRAMES
#define RECEIVE_PACKET_QUEUE_SIZE 256u
#define SEND_PACKET_QUEUE_SIZE 256u

typedef struct {
    uint16_t sequence;
    uint32_t source_frame_id;
    uint64_t capture_time_ms;
    uint64_t send_time_ms;
    uint64_t arrival_time_ms;
    int64_t round_trip_ms;
    uint64_t arrival_gap_ms;
    uint64_t capture_gap_ms;
    size_t payload_length;
    int continuous;
    int source_continuous;
    int has_source_frame_id;
    int has_send_time_ms;
    char status[16];
} android_receive_packet_event;

typedef struct {
    uint16_t sequence;
    uint32_t rtp_timestamp;
    uint32_t source_frame_id;
    uint64_t capture_time_ms;
    uint64_t send_time_ms;
    size_t payload_length;
    int marker;
    int succeeded;
    int error_code;
} android_send_packet_event;

typedef struct {
    int16_t pcm[PTT_AUDIO_FRAME_SAMPLES];
    size_t sample_count;
    ptt_audio_playout_info playout;
} android_playback_frame;

typedef struct {
    pthread_mutex_t mutex;
    pthread_mutex_t client_mutex;
    pthread_mutex_t audio_mutex;
    pthread_mutex_t event_mutex;
    pthread_t thread;
    pthread_t playout_thread;
    int thread_started;
    int playout_thread_started;
    int running;
    int self_loopback;
    char network_type[16];
    unsigned int client_waiters;
    ptt_client *client;
    ptt_audio_encoder *encoder;
    ptt_audio_decoder *decoder;
    android_playback_frame playback_queue[PLAYBACK_QUEUE_FRAMES];
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
    uint64_t rx_stats_started_ms;
    uint64_t rx_stats_packets;
    uint64_t rx_stats_decoded_frames;
    uint64_t rx_stats_decoded_samples;
    uint64_t rx_stats_decode_errors;
    uint64_t rx_stats_dropped_samples;
    uint64_t rx_stats_playback_trim_events;
    uint64_t rx_stats_playback_trimmed_frames;
    uint64_t rx_stats_played_frames;
    uint16_t rx_stats_last_played_sequence;
    int rx_stats_have_last_played_sequence;
    size_t rx_stats_queue_high_water_samples;
    android_receive_packet_event receive_packet_queue[RECEIVE_PACKET_QUEUE_SIZE];
    size_t receive_packet_read;
    size_t receive_packet_write;
    size_t receive_packet_count;
    android_send_packet_event send_packet_queue[SEND_PACKET_QUEUE_SIZE];
    size_t send_packet_read;
    size_t send_packet_write;
    size_t send_packet_count;
} android_ptt_context;

static android_ptt_context g_context = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .client_mutex = PTHREAD_MUTEX_INITIALIZER,
    .audio_mutex = PTHREAD_MUTEX_INITIALIZER,
    .event_mutex = PTHREAD_MUTEX_INITIALIZER,
    .thread_started = 0,
    .playout_thread_started = 0,
    .running = 0,
    .self_loopback = 0,
    .network_type = "unknown",
    .client_waiters = 0,
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

static uint64_t lock_client_for_call(const char *operation)
{
    uint64_t started_ms;
    uint64_t waited_ms;

    pthread_mutex_lock(&g_context.mutex);
    ++g_context.client_waiters;
    pthread_mutex_unlock(&g_context.mutex);

    started_ms = monotonic_ms();
    pthread_mutex_lock(&g_context.client_mutex);
    waited_ms = monotonic_ms() - started_ms;

    pthread_mutex_lock(&g_context.mutex);
    if (g_context.client_waiters > 0) {
        --g_context.client_waiters;
    }
    pthread_mutex_unlock(&g_context.mutex);

    if (waited_ms >= 20u) {
        __android_log_print(ANDROID_LOG_WARN,
                            LOG_TAG,
                            "[client-lock] operation=%s wait_ms=%llu",
                            operation != NULL ? operation : "unknown",
                            (unsigned long long)waited_ms);
    }
    return waited_ms;
}

static void yield_to_client_waiters(void)
{
    struct timespec pause_time;
    unsigned int waiters;

    pthread_mutex_lock(&g_context.mutex);
    waiters = g_context.client_waiters;
    pthread_mutex_unlock(&g_context.mutex);
    if (waiters == 0) {
        return;
    }

    /*
     * pthread mutexes do not guarantee fairness. Without this short handoff
     * window the poll loop can immediately reacquire client_mutex forever,
     * starving UI control, microphone capture and shutdown calls.
     */
    pause_time.tv_sec = 0;
    pause_time.tv_nsec = 1000000L;
    while (nanosleep(&pause_time, &pause_time) != 0 && errno == EINTR) {
    }
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

static void log_tx_stats_locked(uint64_t now_ms,
                                int force,
                                const char *reason)
{
    double average_payload = 0.0;
    uint64_t duration_ms;
    const char *window_name;

    if (g_context.tx_stats_started_ms == 0) {
        reset_tx_stats_locked(now_ms);
        return;
    }
    duration_ms = now_ms >= g_context.tx_stats_started_ms
                      ? now_ms - g_context.tx_stats_started_ms
                      : 0u;
    if (!force && duration_ms < 1000u) {
        return;
    }
    if (force && g_context.tx_stats_input_frames == 0 &&
        g_context.tx_stats_encoded_packets == 0 &&
        g_context.tx_stats_encode_errors == 0 &&
        g_context.tx_stats_send_errors == 0 &&
        g_context.tx_stats_not_ready == 0) {
        reset_tx_stats_locked(now_ms);
        return;
    }
    if (g_context.tx_stats_encoded_packets > 0) {
        average_payload =
            (double)g_context.tx_stats_encoded_bytes /
            (double)g_context.tx_stats_encoded_packets;
    }
    window_name = force ? "audio-tx/final" : "audio-tx/1s";
    __android_log_print(
        ANDROID_LOG_INFO,
        LOG_TAG,
        "[%s] reason=%s duration_ms=%llu network=%s "
        "input_frames=%llu input_samples=%llu "
        "encoded_packets=%llu encoded_bytes=%llu payload_bytes avg=%.1f min=%zu max=%zu "
        "encode_errors=%llu send_errors=%llu not_ready=%llu",
        window_name,
        reason != NULL ? reason : "unknown",
        (unsigned long long)duration_ms,
        g_context.network_type,
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

static void maybe_log_tx_stats_locked(uint64_t now_ms)
{
    log_tx_stats_locked(now_ms, 0, "periodic");
}

static void android_log(void *user_data, const char *message)
{
    (void)user_data;
    if (message == NULL || strstr(message, "[frame-rx]") != NULL ||
        strstr(message, "[frame-tx]") != NULL) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", message);
}

static void receive_packet_event(
    void *user_data,
    const ptt_client_receive_packet_info *packet_info)
{
    android_receive_packet_event *event;

    (void)user_data;
    if (packet_info == NULL) {
        return;
    }
    pthread_mutex_lock(&g_context.event_mutex);
    if (g_context.receive_packet_count == RECEIVE_PACKET_QUEUE_SIZE) {
        g_context.receive_packet_read =
            (g_context.receive_packet_read + 1u) % RECEIVE_PACKET_QUEUE_SIZE;
        --g_context.receive_packet_count;
    }
    event = &g_context.receive_packet_queue[g_context.receive_packet_write];
    event->sequence = packet_info->sequence;
    event->source_frame_id = packet_info->source_frame_id;
    event->capture_time_ms = packet_info->capture_time_ms;
    event->send_time_ms = packet_info->send_time_ms;
    event->arrival_time_ms = packet_info->arrival_time_ms;
    event->round_trip_ms = packet_info->transport_delay_ms;
    event->arrival_gap_ms = packet_info->arrival_gap_ms;
    event->capture_gap_ms = packet_info->capture_gap_ms;
    event->payload_length = packet_info->payload_length;
    event->continuous = packet_info->continuous;
    event->source_continuous = packet_info->source_continuous;
    event->has_source_frame_id = packet_info->has_source_frame_id;
    event->has_send_time_ms = packet_info->has_send_time_ms;
    snprintf(event->status,
             sizeof(event->status),
             "%s",
             packet_info->status != NULL ? packet_info->status : "unknown");
    g_context.receive_packet_write =
        (g_context.receive_packet_write + 1u) % RECEIVE_PACKET_QUEUE_SIZE;
    ++g_context.receive_packet_count;
    pthread_mutex_unlock(&g_context.event_mutex);
    if (g_context.self_loopback) {
        __android_log_print(
            ANDROID_LOG_INFO,
            LOG_TAG,
            "[loopback-rx] capture_frame=%u seq=%u phone_send_ms=%llu "
            "phone_receive_ms=%llu rtt_ms=%lld payload=%zu continuous=%d "
            "source_continuous=%d status=%s",
            packet_info->source_frame_id,
            packet_info->sequence,
            (unsigned long long)(packet_info->has_send_time_ms
                                     ? packet_info->send_time_ms
                                     : 0u),
            (unsigned long long)packet_info->arrival_time_ms,
            (long long)packet_info->transport_delay_ms,
            packet_info->payload_length,
            packet_info->continuous,
            packet_info->source_continuous,
            packet_info->status != NULL ? packet_info->status : "unknown");
    }
}

static void send_packet_event(
    void *user_data,
    const ptt_client_send_packet_info *packet_info)
{
    android_send_packet_event *event;

    (void)user_data;
    if (packet_info == NULL) {
        return;
    }
    pthread_mutex_lock(&g_context.event_mutex);
    if (g_context.send_packet_count == SEND_PACKET_QUEUE_SIZE) {
        g_context.send_packet_read =
            (g_context.send_packet_read + 1u) % SEND_PACKET_QUEUE_SIZE;
        --g_context.send_packet_count;
    }
    event = &g_context.send_packet_queue[g_context.send_packet_write];
    event->sequence = packet_info->sequence;
    event->rtp_timestamp = packet_info->rtp_timestamp;
    event->source_frame_id = packet_info->source_frame_id;
    event->capture_time_ms = packet_info->capture_time_ms;
    event->send_time_ms = packet_info->send_time_ms;
    event->payload_length = packet_info->payload_length;
    event->marker = packet_info->marker;
    event->succeeded = packet_info->succeeded;
    event->error_code = packet_info->error_code;
    g_context.send_packet_write =
        (g_context.send_packet_write + 1u) % SEND_PACKET_QUEUE_SIZE;
    ++g_context.send_packet_count;
    pthread_mutex_unlock(&g_context.event_mutex);
    __android_log_print(packet_info->succeeded ? ANDROID_LOG_INFO
                                               : ANDROID_LOG_ERROR,
                        LOG_TAG,
                        "[frame-tx-local] capture_frame=%u seq=%u "
                        "capture_ms=%llu send_ms=%llu payload=%zu marker=%d "
                        "network=%s result=%s errno=%d",
                        packet_info->source_frame_id,
                        packet_info->sequence,
                        (unsigned long long)packet_info->capture_time_ms,
                        (unsigned long long)packet_info->send_time_ms,
                        packet_info->payload_length,
                        packet_info->marker,
                        g_context.network_type,
                        packet_info->succeeded ? "sent" : "send_error",
                        packet_info->error_code);
}

static void enqueue_playback_locked(const int16_t *pcm,
                                    size_t sample_count,
                                    const ptt_audio_playout_info *playout)
{
    android_playback_frame *frame;
    size_t trimmed_frames;

    if (sample_count > PTT_AUDIO_FRAME_SAMPLES) {
        g_context.rx_stats_dropped_samples +=
            sample_count - PTT_AUDIO_FRAME_SAMPLES;
        pcm += sample_count - PTT_AUDIO_FRAME_SAMPLES;
        sample_count = PTT_AUDIO_FRAME_SAMPLES;
    }
    if (g_context.playback_count == PLAYBACK_QUEUE_FRAMES) {
        trimmed_frames =
            g_context.playback_count - PLAYBACK_QUEUE_RETAIN_FRAMES;
        g_context.playback_read =
            (g_context.playback_read + trimmed_frames) % PLAYBACK_QUEUE_FRAMES;
        g_context.playback_count -= trimmed_frames;
        g_context.rx_stats_dropped_samples +=
            trimmed_frames * PTT_AUDIO_FRAME_SAMPLES;
        ++g_context.rx_stats_playback_trim_events;
        g_context.rx_stats_playback_trimmed_frames += trimmed_frames;
    }
    frame = &g_context.playback_queue[g_context.playback_write];
    memcpy(frame->pcm, pcm, sample_count * sizeof(int16_t));
    frame->sample_count = sample_count;
    if (playout != NULL) {
        frame->playout = *playout;
    } else {
        memset(&frame->playout, 0, sizeof(frame->playout));
    }
    g_context.playback_write =
        (g_context.playback_write + 1u) % PLAYBACK_QUEUE_FRAMES;
    ++g_context.playback_count;
    if (g_context.playback_count * PTT_AUDIO_FRAME_SAMPLES >
        g_context.rx_stats_queue_high_water_samples) {
        g_context.rx_stats_queue_high_water_samples =
            g_context.playback_count * PTT_AUDIO_FRAME_SAMPLES;
    }
}

static void maybe_log_rx_stats_locked(uint64_t now_ms)
{
    ptt_audio_jitter_stats jitter;
    double queue_current_ms;
    double queue_max_ms;

    if (g_context.rx_stats_started_ms == 0) {
        g_context.rx_stats_started_ms = now_ms;
        return;
    }
    if (now_ms - g_context.rx_stats_started_ms < 1000u) {
        return;
    }
    memset(&jitter, 0, sizeof(jitter));
    ptt_audio_decoder_get_jitter_stats(g_context.decoder, &jitter, 1);
    queue_current_ms =
        (double)(g_context.playback_count * PTT_AUDIO_FRAME_SAMPLES) * 1000.0 /
        PTT_AUDIO_SAMPLE_RATE;
    queue_max_ms =
        (double)g_context.rx_stats_queue_high_water_samples * 1000.0 /
        PTT_AUDIO_SAMPLE_RATE;
    __android_log_print(
        ANDROID_LOG_INFO,
        LOG_TAG,
        "[audio-rx/1s] opus_packets=%llu decoded_frames=%llu pcm_samples=%llu "
        "jitter_ms current=%zu max=%zu fec=%llu plc=%llu duplicate=%llu "
        "late=%llu resets=%llu overflow_trim_events=%llu "
        "overflow_trim_old=%llu catchup_drop=%llu "
        "queue_ms current=%.1f max=%.1f target=%u hard=%u "
        "pcm_trim_events=%llu pcm_trim_old=%llu played=%llu last_seq=%u "
        "queue_dropped_samples=%llu decode_errors=%llu",
        (unsigned long long)g_context.rx_stats_packets,
        (unsigned long long)g_context.rx_stats_decoded_frames,
        (unsigned long long)g_context.rx_stats_decoded_samples,
        jitter.buffered_packets * PTT_AUDIO_FRAME_MS,
        jitter.maximum_buffered_packets * PTT_AUDIO_FRAME_MS,
        (unsigned long long)jitter.fec_frames,
        (unsigned long long)jitter.plc_frames,
        (unsigned long long)jitter.duplicate_packets,
        (unsigned long long)jitter.late_packets,
        (unsigned long long)jitter.reset_events,
        (unsigned long long)jitter.overflow_trim_events,
        (unsigned long long)jitter.overflow_dropped_packets,
        (unsigned long long)jitter.catchup_dropped_frames,
        queue_current_ms,
        queue_max_ms,
        PTT_AUDIO_QUEUE_TARGET_MS,
        PTT_AUDIO_QUEUE_HARD_LIMIT_MS,
        (unsigned long long)g_context.rx_stats_playback_trim_events,
        (unsigned long long)g_context.rx_stats_playback_trimmed_frames,
        (unsigned long long)g_context.rx_stats_played_frames,
        g_context.rx_stats_have_last_played_sequence
            ? g_context.rx_stats_last_played_sequence
            : 0u,
        (unsigned long long)g_context.rx_stats_dropped_samples,
        (unsigned long long)(g_context.rx_stats_decode_errors + jitter.decode_errors));
    g_context.rx_stats_started_ms = now_ms;
    g_context.rx_stats_packets = 0;
    g_context.rx_stats_decoded_frames = 0;
    g_context.rx_stats_decoded_samples = 0;
    g_context.rx_stats_decode_errors = 0;
    g_context.rx_stats_dropped_samples = 0;
    g_context.rx_stats_playback_trim_events = 0;
    g_context.rx_stats_playback_trimmed_frames = 0;
    g_context.rx_stats_played_frames = 0;
    g_context.rx_stats_have_last_played_sequence = 0;
    g_context.rx_stats_queue_high_water_samples =
        g_context.playback_count * PTT_AUDIO_FRAME_SAMPLES;
}

static void receive_opus(void *user_data,
                         const uint8_t *payload,
                         size_t payload_length,
                         uint16_t sequence,
                         int marker)
{
    int push_result;
    uint64_t now_ms;

    (void)user_data;
    now_ms = monotonic_ms();
    pthread_mutex_lock(&g_context.audio_mutex);
    if (g_context.decoder == NULL) {
        pthread_mutex_unlock(&g_context.audio_mutex);
        return;
    }
    ++g_context.rx_stats_packets;
    push_result = ptt_audio_jitter_push(g_context.decoder,
                                        payload,
                                        payload_length,
                                        sequence,
                                        marker);
    if (push_result < 0) {
        ++g_context.rx_stats_decode_errors;
    }
    maybe_log_rx_stats_locked(now_ms);
    pthread_mutex_unlock(&g_context.audio_mutex);
}

static void add_deadline_ms(struct timespec *deadline,
                            unsigned int milliseconds)
{
    deadline->tv_sec += (time_t)(milliseconds / 1000u);
    deadline->tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void *playout_clock_thread(void *argument)
{
    struct timespec deadline;
    int16_t decoded[PTT_AUDIO_FRAME_SAMPLES];

    (void)argument;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    add_deadline_ms(&deadline, PTT_AUDIO_FRAME_MS);
    for (;;) {
        int keep_running;
        int samples;
        ptt_audio_playout_info playout;

        while (clock_nanosleep(CLOCK_MONOTONIC,
                               TIMER_ABSTIME,
                               &deadline,
                               NULL) == EINTR) {
        }
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        add_deadline_ms(&deadline, PTT_AUDIO_FRAME_MS);

        pthread_mutex_lock(&g_context.mutex);
        keep_running = g_context.running;
        pthread_mutex_unlock(&g_context.mutex);
        if (!keep_running) {
            break;
        }

        pthread_mutex_lock(&g_context.audio_mutex);
        if (g_context.decoder == NULL) {
            pthread_mutex_unlock(&g_context.audio_mutex);
            continue;
        }
        samples = ptt_audio_jitter_pull_ex(g_context.decoder,
                                           decoded,
                                           PTT_AUDIO_FRAME_SAMPLES,
                                           &playout);
        if (samples > 0) {
            ++g_context.rx_stats_decoded_frames;
            g_context.rx_stats_decoded_samples += (uint64_t)samples;
            enqueue_playback_locked(decoded, (size_t)samples, &playout);
        } else if (samples < 0) {
            ++g_context.rx_stats_decode_errors;
        }
        maybe_log_rx_stats_locked(monotonic_ms());
        pthread_mutex_unlock(&g_context.audio_mutex);
    }
    return NULL;
}

static void *network_thread(void *argument)
{
    ptt_client *client;
    int keep_running;
    int result;

    (void)argument;
    pthread_mutex_lock(&g_context.client_mutex);
    pthread_mutex_lock(&g_context.mutex);
    keep_running = g_context.running && g_context.client != NULL;
    client = g_context.client;
    pthread_mutex_unlock(&g_context.mutex);
    if (!keep_running) {
        pthread_mutex_unlock(&g_context.client_mutex);
        return NULL;
    }
    result = ptt_client_start(client);
    pthread_mutex_unlock(&g_context.client_mutex);
    if (result != 0) {
        pthread_mutex_lock(&g_context.mutex);
        g_context.running = 0;
        pthread_mutex_unlock(&g_context.mutex);
        return NULL;
    }

    for (;;) {
        /*
         * ptt_client is not thread-safe, so its calls are serialized by the
         * dedicated client mutex.  Do not hold the lifecycle mutex while
         * poll() waits: the playout clock and UI control path need that mutex
         * for only a few instructions and must never be starved by networking.
         */
        pthread_mutex_lock(&g_context.client_mutex);
        pthread_mutex_lock(&g_context.mutex);
        keep_running = g_context.running && g_context.client != NULL;
        client = g_context.client;
        pthread_mutex_unlock(&g_context.mutex);
        if (keep_running) {
            result = ptt_client_poll(client, 5);
        } else {
            result = 0;
        }
        pthread_mutex_unlock(&g_context.client_mutex);
        yield_to_client_waiters();

        if (result != 0) {
            pthread_mutex_lock(&g_context.mutex);
            g_context.running = 0;
            pthread_mutex_unlock(&g_context.mutex);
            keep_running = 0;
        }

        if (!keep_running) {
            break;
        }
    }
    return NULL;
}

static void shutdown_context(void)
{
    pthread_t thread;
    pthread_t playout_thread;
    int should_join;
    int should_join_playout;
    ptt_audio_encoder *encoder;
    ptt_audio_decoder *decoder;

    pthread_mutex_lock(&g_context.mutex);
    g_context.running = 0;
    g_context.self_loopback = 0;
    should_join = g_context.thread_started;
    thread = g_context.thread;
    should_join_playout = g_context.playout_thread_started;
    playout_thread = g_context.playout_thread;
    pthread_mutex_unlock(&g_context.mutex);

    if (should_join && !pthread_equal(pthread_self(), thread)) {
        (void)pthread_join(thread, NULL);
    }
    if (should_join_playout && !pthread_equal(pthread_self(), playout_thread)) {
        (void)pthread_join(playout_thread, NULL);
    }

    /* Exclude any in-flight status/PTT/capture JNI call before destruction. */
    pthread_mutex_lock(&g_context.client_mutex);
    pthread_mutex_lock(&g_context.mutex);
    g_context.thread_started = 0;
    g_context.playout_thread_started = 0;
    encoder = g_context.encoder;
    g_context.encoder = NULL;
    if (g_context.client != NULL) {
        ptt_client_destroy(g_context.client);
        g_context.client = NULL;
    }
    reset_tx_stats_locked(0);
    pthread_mutex_unlock(&g_context.mutex);
    pthread_mutex_unlock(&g_context.client_mutex);

    pthread_mutex_lock(&g_context.audio_mutex);
    decoder = g_context.decoder;
    g_context.decoder = NULL;
    g_context.playback_read = 0;
    g_context.playback_write = 0;
    g_context.playback_count = 0;
    pthread_mutex_unlock(&g_context.audio_mutex);

    pthread_mutex_lock(&g_context.event_mutex);
    g_context.receive_packet_read = 0;
    g_context.receive_packet_write = 0;
    g_context.receive_packet_count = 0;
    g_context.send_packet_read = 0;
    g_context.send_packet_write = 0;
    g_context.send_packet_count = 0;
    pthread_mutex_unlock(&g_context.event_mutex);

    ptt_audio_encoder_destroy(encoder);
    ptt_audio_decoder_destroy(decoder);
}

JNIEXPORT jboolean JNICALL
Java_com_master_sipptt_NativePtt_nativeStart(JNIEnv *env,
                                              jclass clazz,
                                              jstring server,
                                              jstring device_id,
                                              jstring group_id,
                                              jboolean self_loopback)
{
    const char *server_text;
    const char *device_text;
    const char *group_text;
    ptt_client_config config;
    ptt_client *client;
    ptt_audio_encoder *encoder;
    ptt_audio_decoder *decoder;
    int thread_result;
    int playout_thread_result;

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
    config.self_loopback = self_loopback == JNI_TRUE;
    config.log_fn = android_log;
    config.log_user_data = NULL;
    config.opus_receive_fn = receive_opus;
    config.opus_receive_user_data = NULL;
    config.receive_packet_fn = receive_packet_event;
    config.receive_packet_user_data = NULL;
    config.send_packet_fn = send_packet_event;
    config.send_packet_user_data = NULL;

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
    g_context.self_loopback = config.self_loopback;
    reset_tx_stats_locked(monotonic_ms());
    pthread_mutex_lock(&g_context.audio_mutex);
    g_context.decoder = decoder;
    g_context.playback_read = 0;
    g_context.playback_write = 0;
    g_context.playback_count = 0;
    g_context.rx_stats_started_ms = 0;
    g_context.rx_stats_packets = 0;
    g_context.rx_stats_decoded_frames = 0;
    g_context.rx_stats_decoded_samples = 0;
    g_context.rx_stats_decode_errors = 0;
    g_context.rx_stats_dropped_samples = 0;
    g_context.rx_stats_playback_trim_events = 0;
    g_context.rx_stats_playback_trimmed_frames = 0;
    g_context.rx_stats_played_frames = 0;
    g_context.rx_stats_last_played_sequence = 0;
    g_context.rx_stats_have_last_played_sequence = 0;
    g_context.rx_stats_queue_high_water_samples = 0;
    pthread_mutex_unlock(&g_context.audio_mutex);
    pthread_mutex_lock(&g_context.event_mutex);
    g_context.receive_packet_read = 0;
    g_context.receive_packet_write = 0;
    g_context.receive_packet_count = 0;
    g_context.send_packet_read = 0;
    g_context.send_packet_write = 0;
    g_context.send_packet_count = 0;
    pthread_mutex_unlock(&g_context.event_mutex);
    g_context.running = 1;
    thread_result = pthread_create(&g_context.thread, NULL, network_thread, NULL);
    if (thread_result == 0) {
        g_context.thread_started = 1;
    } else {
        g_context.running = 0;
    }
    playout_thread_result = thread_result == 0
                                ? pthread_create(&g_context.playout_thread,
                                                 NULL,
                                                 playout_clock_thread,
                                                 NULL)
                                : -1;
    if (playout_thread_result == 0) {
        g_context.playout_thread_started = 1;
    } else {
        g_context.running = 0;
    }
    pthread_mutex_unlock(&g_context.mutex);

    if (thread_result != 0 || playout_thread_result != 0) {
        shutdown_context();
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO,
                        LOG_TAG,
                        "[version] Android native core=%s loopback=%d",
                        PTT_PRODUCT_VERSION,
                        config.self_loopback);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_master_sipptt_NativePtt_nativeRequestTalk(JNIEnv *env, jclass clazz)
{
    ptt_client *client;
    int result = -1;

    (void)env;
    (void)clazz;
    (void)lock_client_for_call("request-talk");
    pthread_mutex_lock(&g_context.mutex);
    client = g_context.running ? g_context.client : NULL;
    pthread_mutex_unlock(&g_context.mutex);
    if (client != NULL) {
        result = ptt_client_request_talk(client);
    }
    pthread_mutex_unlock(&g_context.client_mutex);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_master_sipptt_NativePtt_nativeReleaseTalk(JNIEnv *env, jclass clazz)
{
    ptt_client *client;
    int result = -1;

    (void)env;
    (void)clazz;
    (void)lock_client_for_call("release-talk");
    pthread_mutex_lock(&g_context.mutex);
    client = g_context.running ? g_context.client : NULL;
    pthread_mutex_unlock(&g_context.mutex);
    if (client != NULL) {
        result = ptt_client_release_talk(client);
    }
    pthread_mutex_unlock(&g_context.client_mutex);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_master_sipptt_NativePtt_nativeFlushTxDiagnostics(JNIEnv *env,
                                                           jclass clazz,
                                                           jstring reason)
{
    const char *reason_text = NULL;

    (void)clazz;
    if (reason != NULL) {
        reason_text = (*env)->GetStringUTFChars(env, reason, NULL);
    }
    (void)lock_client_for_call("flush-tx-diagnostics");
    log_tx_stats_locked(monotonic_ms(),
                        1,
                        reason_text != NULL ? reason_text : "manual");
    pthread_mutex_unlock(&g_context.client_mutex);
    if (reason_text != NULL) {
        (*env)->ReleaseStringUTFChars(env, reason, reason_text);
    }
}

JNIEXPORT void JNICALL
Java_com_master_sipptt_NativePtt_nativeSetNetworkType(JNIEnv *env,
                                                       jclass clazz,
                                                       jstring network_type)
{
    const char *network_text = NULL;

    (void)clazz;
    if (network_type != NULL) {
        network_text = (*env)->GetStringUTFChars(env, network_type, NULL);
    }
    pthread_mutex_lock(&g_context.client_mutex);
    snprintf(g_context.network_type,
             sizeof(g_context.network_type),
             "%s",
             network_text != NULL ? network_text : "unknown");
    pthread_mutex_unlock(&g_context.client_mutex);
    if (network_text != NULL) {
        (*env)->ReleaseStringUTFChars(env, network_type, network_text);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_master_sipptt_NativePtt_nativePushCapturePcm(JNIEnv *env,
                                                       jclass clazz,
                                                       jshortArray pcm,
                                                       jint samples,
                                                       jlong capture_frame_id,
                                                       jlong capture_time_ms)
{
    ptt_client *client;
    ptt_audio_encoder *encoder;
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
    (void)lock_client_for_call("capture-send");
    pthread_mutex_lock(&g_context.mutex);
    client = g_context.running ? g_context.client : NULL;
    encoder = g_context.running ? g_context.encoder : NULL;
    pthread_mutex_unlock(&g_context.mutex);
    ++g_context.tx_stats_input_frames;
    g_context.tx_stats_input_samples += (uint64_t)samples;
    if (client != NULL && encoder != NULL) {
        encoded = ptt_audio_encode_pcm48(encoder,
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
            result = ptt_client_send_opus_frame(
                client,
                packet,
                (size_t)encoded,
                (uint32_t)capture_frame_id,
                (uint64_t)capture_time_ms,
                NULL);
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
    pthread_mutex_unlock(&g_context.client_mutex);
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_master_sipptt_NativePtt_nativeReadPlaybackPcm(JNIEnv *env,
                                                        jclass clazz,
                                                        jshortArray pcm,
                                                        jint max_samples)
{
    android_playback_frame frame;
    size_t requested;
    size_t queued_frames;
    int have_frame = 0;

    (void)clazz;
    if (pcm == NULL || max_samples <= 0) {
        return 0;
    }
    requested = (size_t)max_samples;
    if (requested > (size_t)(*env)->GetArrayLength(env, pcm)) {
        requested = (size_t)(*env)->GetArrayLength(env, pcm);
    }
    pthread_mutex_lock(&g_context.audio_mutex);
    if (g_context.playback_count > 0) {
        android_playback_frame *queued =
            &g_context.playback_queue[g_context.playback_read];
        if (requested >= queued->sample_count) {
            frame = *queued;
            have_frame = 1;
        }
    }
    if (have_frame) {
        g_context.playback_read =
            (g_context.playback_read + 1u) % PLAYBACK_QUEUE_FRAMES;
        --g_context.playback_count;
        ++g_context.rx_stats_played_frames;
        g_context.rx_stats_last_played_sequence = frame.playout.sequence;
        g_context.rx_stats_have_last_played_sequence = 1;
    }
    queued_frames = g_context.playback_count;
    pthread_mutex_unlock(&g_context.audio_mutex);

    if (!have_frame) {
        return 0;
    }
    (*env)->SetShortArrayRegion(env,
                                pcm,
                                0,
                                (jsize)frame.sample_count,
                                (const jshort *)frame.pcm);
    if ((*env)->ExceptionCheck(env)) {
        return 0;
    }
    (void)queued_frames;
    return (jint)frame.sample_count;
}

JNIEXPORT jstring JNICALL
Java_com_master_sipptt_NativePtt_nativeGetStatus(JNIEnv *env, jclass clazz)
{
    ptt_client *client;
    ptt_client_snapshot snapshot;
    char status[768];

    (void)clazz;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.mode = PTT_CLIENT_STOPPED;
    snprintf(snapshot.detail, sizeof(snapshot.detail), "%s", "尚未加入组队");

    (void)lock_client_for_call("get-status");
    pthread_mutex_lock(&g_context.mutex);
    client = g_context.client;
    pthread_mutex_unlock(&g_context.mutex);
    if (client != NULL) {
        ptt_client_get_snapshot(client, &snapshot);
    }
    pthread_mutex_unlock(&g_context.client_mutex);

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

JNIEXPORT jstring JNICALL
Java_com_master_sipptt_NativePtt_nativeDrainReceivePackets(JNIEnv *env,
                                                            jclass clazz)
{
    char output[32768];
    size_t used = 0;

    (void)clazz;
    output[0] = '\0';
    pthread_mutex_lock(&g_context.event_mutex);
    while (g_context.receive_packet_count > 0) {
        const android_receive_packet_event *event =
            &g_context.receive_packet_queue[g_context.receive_packet_read];
        int written = snprintf(output + used,
                               sizeof(output) - used,
                               "%u\t%u\t%llu\t%llu\t%llu\t%lld\t%llu\t%llu\t%zu\t%d\t%d\t%d\t%d\t%s\n",
                               event->sequence,
                               event->source_frame_id,
                               (unsigned long long)event->capture_time_ms,
                               (unsigned long long)event->send_time_ms,
                               (unsigned long long)event->arrival_time_ms,
                               (long long)event->round_trip_ms,
                               (unsigned long long)event->arrival_gap_ms,
                               (unsigned long long)event->capture_gap_ms,
                               event->payload_length,
                               event->continuous,
                               event->source_continuous,
                               event->has_source_frame_id,
                               event->has_send_time_ms,
                               event->status);

        if (written < 0 || (size_t)written >= sizeof(output) - used) {
            break;
        }
        used += (size_t)written;
        g_context.receive_packet_read =
            (g_context.receive_packet_read + 1u) % RECEIVE_PACKET_QUEUE_SIZE;
        --g_context.receive_packet_count;
    }
    pthread_mutex_unlock(&g_context.event_mutex);
    return (*env)->NewStringUTF(env, output);
}

JNIEXPORT jstring JNICALL
Java_com_master_sipptt_NativePtt_nativeDrainSendPackets(JNIEnv *env,
                                                         jclass clazz)
{
    char output[32768];
    size_t used = 0;

    (void)clazz;
    output[0] = '\0';
    pthread_mutex_lock(&g_context.event_mutex);
    while (g_context.send_packet_count > 0) {
        const android_send_packet_event *event =
            &g_context.send_packet_queue[g_context.send_packet_read];
        int written = snprintf(output + used,
                               sizeof(output) - used,
                               "%u\t%u\t%u\t%llu\t%llu\t%zu\t%d\t%d\t%d\n",
                               event->sequence,
                               event->rtp_timestamp,
                               event->source_frame_id,
                               (unsigned long long)event->capture_time_ms,
                               (unsigned long long)event->send_time_ms,
                               event->payload_length,
                               event->marker,
                               event->succeeded,
                               event->error_code);

        if (written < 0 || (size_t)written >= sizeof(output) - used) {
            break;
        }
        used += (size_t)written;
        g_context.send_packet_read =
            (g_context.send_packet_read + 1u) % SEND_PACKET_QUEUE_SIZE;
        --g_context.send_packet_count;
    }
    pthread_mutex_unlock(&g_context.event_mutex);
    return (*env)->NewStringUTF(env, output);
}

JNIEXPORT void JNICALL
Java_com_master_sipptt_NativePtt_nativeShutdown(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    shutdown_context();
}
