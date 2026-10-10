#include "ptt_pc_audio.h"

#include "ptt_audio.h"

#include <errno.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OPUS_QUEUE_CAPACITY 64u
#define PCM_QUEUE_CAPACITY (PTT_AUDIO_SAMPLE_RATE * 2u)
#define PLAYBACK_IDLE_CLOSE_MS 500u

typedef struct {
    size_t length;
    uint8_t data[PTT_OPUS_MAX_PACKET_BYTES];
} opus_queue_item;

struct ptt_pc_audio {
    pthread_mutex_t mutex;
    pthread_cond_t capture_condition;
    pthread_cond_t playback_condition;
    pthread_t capture_thread;
    pthread_t playback_thread;
    int capture_thread_started;
    int playback_thread_started;
    int running;
    int talking;
    ptt_audio_encoder *encoder;
    ptt_audio_decoder *decoder;
    opus_queue_item opus_queue[OPUS_QUEUE_CAPACITY];
    size_t opus_read;
    size_t opus_write;
    size_t opus_count;
    int16_t pcm_queue[PCM_QUEUE_CAPACITY];
    size_t pcm_read;
    size_t pcm_write;
    size_t pcm_count;
    uint64_t rx_stats_started_ms;
    uint64_t rx_stats_packets;
    uint64_t rx_stats_payload_bytes;
    size_t rx_stats_payload_min_bytes;
    size_t rx_stats_payload_max_bytes;
    uint64_t rx_stats_decoded_frames;
    uint64_t rx_stats_decode_errors;
    uint64_t rx_stats_decoded_samples;
    uint64_t rx_stats_pcm_abs_sum;
    unsigned int rx_stats_pcm_peak;
    uint64_t rx_stats_dropped_samples;
    size_t rx_stats_queue_high_water_samples;
    uint64_t rx_stats_playback_writes;
    uint64_t rx_stats_playback_samples;
    uint64_t rx_stats_playback_errors;
    uint64_t rx_stats_playback_opens;
    uint64_t rx_stats_playback_idle_closes;
};

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    (void)nanosleep(&delay, NULL);
}

static int context_is_talking(ptt_pc_audio *audio)
{
    int talking;

    pthread_mutex_lock(&audio->mutex);
    talking = audio->running && audio->talking;
    pthread_mutex_unlock(&audio->mutex);
    return talking;
}

static void enqueue_opus(ptt_pc_audio *audio,
                         const uint8_t *packet,
                         size_t packet_length)
{
    opus_queue_item *item;

    pthread_mutex_lock(&audio->mutex);
    if (audio->opus_count == OPUS_QUEUE_CAPACITY) {
        audio->opus_read = (audio->opus_read + 1u) % OPUS_QUEUE_CAPACITY;
        --audio->opus_count;
    }
    item = &audio->opus_queue[audio->opus_write];
    item->length = packet_length;
    memcpy(item->data, packet, packet_length);
    audio->opus_write = (audio->opus_write + 1u) % OPUS_QUEUE_CAPACITY;
    ++audio->opus_count;
    pthread_mutex_unlock(&audio->mutex);
}

static void enqueue_pcm_locked(ptt_pc_audio *audio,
                               const int16_t *pcm,
                               size_t sample_count)
{
    size_t index;

    if (sample_count > PCM_QUEUE_CAPACITY) {
        audio->rx_stats_dropped_samples += sample_count - PCM_QUEUE_CAPACITY;
        pcm += sample_count - PCM_QUEUE_CAPACITY;
        sample_count = PCM_QUEUE_CAPACITY;
    }
    while (audio->pcm_count + sample_count > PCM_QUEUE_CAPACITY) {
        audio->pcm_read = (audio->pcm_read + 1u) % PCM_QUEUE_CAPACITY;
        --audio->pcm_count;
        ++audio->rx_stats_dropped_samples;
    }
    for (index = 0; index < sample_count; ++index) {
        audio->pcm_queue[audio->pcm_write] = pcm[index];
        audio->pcm_write = (audio->pcm_write + 1u) % PCM_QUEUE_CAPACITY;
    }
    audio->pcm_count += sample_count;
    if (audio->pcm_count > audio->rx_stats_queue_high_water_samples) {
        audio->rx_stats_queue_high_water_samples = audio->pcm_count;
    }
    pthread_cond_signal(&audio->playback_condition);
}

static void maybe_log_receive_stats_locked(ptt_pc_audio *audio, uint64_t now_ms)
{
    double average_payload;
    double average_absolute_pcm;
    double queue_high_water_ms;

    if (audio->rx_stats_started_ms == 0) {
        audio->rx_stats_started_ms = now_ms;
        return;
    }
    if (now_ms - audio->rx_stats_started_ms < 1000u ||
        audio->rx_stats_packets == 0) {
        return;
    }

    average_payload =
        (double)audio->rx_stats_payload_bytes /
        (double)audio->rx_stats_packets;
    average_absolute_pcm =
        audio->rx_stats_decoded_samples > 0
            ? (double)audio->rx_stats_pcm_abs_sum /
                  (double)audio->rx_stats_decoded_samples
            : 0.0;
    queue_high_water_ms =
        (double)audio->rx_stats_queue_high_water_samples * 1000.0 /
        (double)PTT_AUDIO_SAMPLE_RATE;

    fprintf(stderr,
            "[audio-rx/1s] opus_packets=%llu payload_bytes avg=%.1f min=%zu max=%zu "
            "decoded_frames=%llu decode_errors=%llu pcm_samples=%llu "
            "pcm_level avg_abs=%.1f peak=%u queue_max=%.1fms dropped_samples=%llu "
            "playback_writes=%llu playback_samples=%llu playback_errors=%llu "
            "stream_opens=%llu idle_closes=%llu\n",
            (unsigned long long)audio->rx_stats_packets,
            average_payload,
            audio->rx_stats_payload_min_bytes,
            audio->rx_stats_payload_max_bytes,
            (unsigned long long)audio->rx_stats_decoded_frames,
            (unsigned long long)audio->rx_stats_decode_errors,
            (unsigned long long)audio->rx_stats_decoded_samples,
            average_absolute_pcm,
            audio->rx_stats_pcm_peak,
            queue_high_water_ms,
            (unsigned long long)audio->rx_stats_dropped_samples,
            (unsigned long long)audio->rx_stats_playback_writes,
            (unsigned long long)audio->rx_stats_playback_samples,
            (unsigned long long)audio->rx_stats_playback_errors,
            (unsigned long long)audio->rx_stats_playback_opens,
            (unsigned long long)audio->rx_stats_playback_idle_closes);

    audio->rx_stats_started_ms = now_ms;
    audio->rx_stats_packets = 0;
    audio->rx_stats_payload_bytes = 0;
    audio->rx_stats_payload_min_bytes = 0;
    audio->rx_stats_payload_max_bytes = 0;
    audio->rx_stats_decoded_frames = 0;
    audio->rx_stats_decode_errors = 0;
    audio->rx_stats_decoded_samples = 0;
    audio->rx_stats_pcm_abs_sum = 0;
    audio->rx_stats_pcm_peak = 0;
    audio->rx_stats_dropped_samples = 0;
    audio->rx_stats_queue_high_water_samples = audio->pcm_count;
    audio->rx_stats_playback_writes = 0;
    audio->rx_stats_playback_samples = 0;
    audio->rx_stats_playback_errors = 0;
    audio->rx_stats_playback_opens = 0;
    audio->rx_stats_playback_idle_closes = 0;
}

static size_t take_pcm_locked(ptt_pc_audio *audio,
                              int16_t *pcm,
                              size_t capacity)
{
    size_t count = audio->pcm_count < capacity ? audio->pcm_count : capacity;
    size_t index;

    for (index = 0; index < count; ++index) {
        pcm[index] = audio->pcm_queue[audio->pcm_read];
        audio->pcm_read = (audio->pcm_read + 1u) % PCM_QUEUE_CAPACITY;
    }
    audio->pcm_count -= count;
    return count;
}

static pa_simple *open_capture(void)
{
    pa_sample_spec sample_spec;
    pa_buffer_attr buffer_attributes;
    int error = 0;

    sample_spec.format = PA_SAMPLE_S16LE;
    sample_spec.rate = PTT_CAPTURE_SAMPLE_RATE;
    sample_spec.channels = PTT_AUDIO_CHANNELS;
    buffer_attributes.maxlength = (uint32_t)-1;
    buffer_attributes.tlength = (uint32_t)-1;
    buffer_attributes.prebuf = (uint32_t)-1;
    buffer_attributes.minreq = (uint32_t)-1;
    buffer_attributes.fragsize = PTT_CAPTURE_FRAME_SAMPLES * sizeof(int16_t);
    return pa_simple_new(NULL,
                         "SIP PTT",
                         PA_STREAM_RECORD,
                         NULL,
                         "PTT microphone",
                         &sample_spec,
                         NULL,
                         &buffer_attributes,
                         &error);
}

static pa_simple *open_playback(void)
{
    pa_sample_spec sample_spec;
    pa_buffer_attr buffer_attributes;
    int error = 0;

    sample_spec.format = PA_SAMPLE_S16LE;
    sample_spec.rate = PTT_AUDIO_SAMPLE_RATE;
    sample_spec.channels = PTT_AUDIO_CHANNELS;
    buffer_attributes.maxlength = (uint32_t)-1;
    buffer_attributes.tlength = PTT_AUDIO_FRAME_SAMPLES * sizeof(int16_t) * 4u;
    buffer_attributes.prebuf = (uint32_t)-1;
    buffer_attributes.minreq = PTT_AUDIO_FRAME_SAMPLES * sizeof(int16_t);
    buffer_attributes.fragsize = (uint32_t)-1;
    return pa_simple_new(NULL,
                         "SIP PTT",
                         PA_STREAM_PLAYBACK,
                         NULL,
                         "PTT speaker",
                         &sample_spec,
                         NULL,
                         &buffer_attributes,
                         &error);
}

static void *capture_main(void *argument)
{
    ptt_pc_audio *audio = (ptt_pc_audio *)argument;
    int16_t capture[PTT_CAPTURE_FRAME_SAMPLES];
    uint8_t packet[PTT_OPUS_MAX_PACKET_BYTES];

    for (;;) {
        pa_simple *stream;

        pthread_mutex_lock(&audio->mutex);
        while (audio->running && !audio->talking) {
            pthread_cond_wait(&audio->capture_condition, &audio->mutex);
        }
        if (!audio->running) {
            pthread_mutex_unlock(&audio->mutex);
            break;
        }
        pthread_mutex_unlock(&audio->mutex);

        stream = open_capture();
        if (stream == NULL) {
            fprintf(stderr, "[audio] cannot open 48 kHz microphone; retrying\n");
            sleep_ms(300u);
            continue;
        }
        fprintf(stderr, "[audio] microphone active: PCM16 mono 48000 Hz\n");
        while (context_is_talking(audio)) {
            int error = 0;
            int encoded;

            if (pa_simple_read(stream, capture, sizeof(capture), &error) < 0) {
                fprintf(stderr, "[audio] microphone read failed: %s\n",
                        pa_strerror(error));
                break;
            }
            encoded = ptt_audio_encode_pcm48(audio->encoder,
                                             capture,
                                             PTT_CAPTURE_FRAME_SAMPLES,
                                             packet,
                                             sizeof(packet));
            if (encoded > 0) {
                enqueue_opus(audio, packet, (size_t)encoded);
            }
        }
        pa_simple_free(stream);
        fprintf(stderr, "[audio] microphone idle\n");
    }
    return NULL;
}

static void deadline_after_ms(struct timespec *deadline, unsigned int milliseconds)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += (time_t)(milliseconds / 1000u);
    deadline->tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void *playback_main(void *argument)
{
    ptt_pc_audio *audio = (ptt_pc_audio *)argument;
    pa_simple *stream = NULL;
    int16_t pcm[PTT_AUDIO_FRAME_SAMPLES];

    for (;;) {
        size_t samples;
        int timed_out = 0;

        pthread_mutex_lock(&audio->mutex);
        while (audio->running && audio->pcm_count == 0) {
            if (stream == NULL) {
                pthread_cond_wait(&audio->playback_condition, &audio->mutex);
            } else {
                struct timespec deadline;
                int wait_result;

                deadline_after_ms(&deadline, PLAYBACK_IDLE_CLOSE_MS);
                wait_result = pthread_cond_timedwait(&audio->playback_condition,
                                                     &audio->mutex,
                                                     &deadline);
                if (wait_result == ETIMEDOUT && audio->pcm_count == 0) {
                    timed_out = 1;
                    break;
                }
            }
        }
        if (!audio->running) {
            pthread_mutex_unlock(&audio->mutex);
            break;
        }
        if (timed_out) {
            ++audio->rx_stats_playback_idle_closes;
            pthread_mutex_unlock(&audio->mutex);
            pa_simple_drain(stream, NULL);
            pa_simple_free(stream);
            stream = NULL;
            continue;
        }
        samples = take_pcm_locked(audio, pcm, PTT_AUDIO_FRAME_SAMPLES);
        pthread_mutex_unlock(&audio->mutex);

        if (samples == 0) {
            continue;
        }
        if (stream == NULL) {
            stream = open_playback();
            if (stream == NULL) {
                fprintf(stderr, "[audio] cannot open 8 kHz speaker\n");
                continue;
            }
            fprintf(stderr, "[audio] speaker active: PCM16 mono 8000 Hz\n");
            pthread_mutex_lock(&audio->mutex);
            ++audio->rx_stats_playback_opens;
            pthread_mutex_unlock(&audio->mutex);
        }
        {
            int error = 0;
            if (pa_simple_write(stream, pcm, samples * sizeof(int16_t), &error) < 0) {
                fprintf(stderr, "[audio] speaker write failed: %s\n",
                        pa_strerror(error));
                pa_simple_free(stream);
                stream = NULL;
                pthread_mutex_lock(&audio->mutex);
                ++audio->rx_stats_playback_errors;
                pthread_mutex_unlock(&audio->mutex);
            } else {
                pthread_mutex_lock(&audio->mutex);
                ++audio->rx_stats_playback_writes;
                audio->rx_stats_playback_samples += samples;
                pthread_mutex_unlock(&audio->mutex);
            }
        }
    }
    if (stream != NULL) {
        pa_simple_drain(stream, NULL);
        pa_simple_free(stream);
    }
    return NULL;
}

ptt_pc_audio *ptt_pc_audio_create(unsigned int opus_bitrate)
{
    ptt_pc_audio *audio = (ptt_pc_audio *)calloc(1, sizeof(*audio));

    if (audio == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&audio->mutex, NULL) != 0 ||
        pthread_cond_init(&audio->capture_condition, NULL) != 0 ||
        pthread_cond_init(&audio->playback_condition, NULL) != 0) {
        free(audio);
        return NULL;
    }
    audio->encoder = ptt_audio_encoder_create(opus_bitrate);
    audio->decoder = ptt_audio_decoder_create();
    if (audio->encoder == NULL || audio->decoder == NULL) {
        ptt_pc_audio_destroy(audio);
        return NULL;
    }
    return audio;
}

int ptt_pc_audio_start(ptt_pc_audio *audio)
{
    if (audio == NULL || audio->running) {
        return -1;
    }
    audio->running = 1;
    if (pthread_create(&audio->capture_thread, NULL, capture_main, audio) != 0) {
        audio->running = 0;
        return -1;
    }
    audio->capture_thread_started = 1;
    if (pthread_create(&audio->playback_thread, NULL, playback_main, audio) != 0) {
        ptt_pc_audio_stop(audio);
        return -1;
    }
    audio->playback_thread_started = 1;
    return 0;
}

void ptt_pc_audio_set_talking(ptt_pc_audio *audio, int talking)
{
    if (audio == NULL) {
        return;
    }
    pthread_mutex_lock(&audio->mutex);
    audio->talking = talking != 0;
    pthread_cond_signal(&audio->capture_condition);
    pthread_mutex_unlock(&audio->mutex);
}

int ptt_pc_audio_take_opus(ptt_pc_audio *audio,
                           uint8_t *packet,
                           size_t capacity,
                           size_t *packet_length)
{
    opus_queue_item *item;

    if (audio == NULL || packet == NULL || packet_length == NULL) {
        return 0;
    }
    pthread_mutex_lock(&audio->mutex);
    if (audio->opus_count == 0) {
        pthread_mutex_unlock(&audio->mutex);
        return 0;
    }
    item = &audio->opus_queue[audio->opus_read];
    if (capacity < item->length) {
        pthread_mutex_unlock(&audio->mutex);
        return 0;
    }
    *packet_length = item->length;
    memcpy(packet, item->data, item->length);
    audio->opus_read = (audio->opus_read + 1u) % OPUS_QUEUE_CAPACITY;
    --audio->opus_count;
    pthread_mutex_unlock(&audio->mutex);
    return 1;
}

void ptt_pc_audio_receive_opus(void *user_data,
                               const uint8_t *payload,
                               size_t payload_length,
                               uint16_t sequence)
{
    ptt_pc_audio *audio = (ptt_pc_audio *)user_data;
    int16_t decoded[PTT_AUDIO_MAX_DECODE_SAMPLES];
    int samples;
    size_t index;
    uint64_t absolute_sum = 0;
    unsigned int peak = 0;
    uint64_t now_ms;

    (void)sequence;
    if (audio == NULL || audio->decoder == NULL) {
        return;
    }
    samples = ptt_audio_decode_opus(audio->decoder,
                                    payload,
                                    payload_length,
                                    decoded,
                                    PTT_AUDIO_MAX_DECODE_SAMPLES);
    if (samples > 0) {
        for (index = 0; index < (size_t)samples; ++index) {
            int value = decoded[index];
            unsigned int absolute =
                (unsigned int)(value < 0 ? -(int64_t)value : value);
            absolute_sum += absolute;
            if (absolute > peak) {
                peak = absolute;
            }
        }
    }

    now_ms = monotonic_ms();
    pthread_mutex_lock(&audio->mutex);
    ++audio->rx_stats_packets;
    audio->rx_stats_payload_bytes += payload_length;
    if (audio->rx_stats_payload_min_bytes == 0 ||
        payload_length < audio->rx_stats_payload_min_bytes) {
        audio->rx_stats_payload_min_bytes = payload_length;
    }
    if (payload_length > audio->rx_stats_payload_max_bytes) {
        audio->rx_stats_payload_max_bytes = payload_length;
    }
    if (samples > 0) {
        ++audio->rx_stats_decoded_frames;
        audio->rx_stats_decoded_samples += (uint64_t)samples;
        audio->rx_stats_pcm_abs_sum += absolute_sum;
        if (peak > audio->rx_stats_pcm_peak) {
            audio->rx_stats_pcm_peak = peak;
        }
        enqueue_pcm_locked(audio, decoded, (size_t)samples);
    } else {
        ++audio->rx_stats_decode_errors;
    }
    maybe_log_receive_stats_locked(audio, now_ms);
    pthread_mutex_unlock(&audio->mutex);
}

void ptt_pc_audio_stop(ptt_pc_audio *audio)
{
    if (audio == NULL) {
        return;
    }
    pthread_mutex_lock(&audio->mutex);
    audio->running = 0;
    audio->talking = 0;
    pthread_cond_broadcast(&audio->capture_condition);
    pthread_cond_broadcast(&audio->playback_condition);
    pthread_mutex_unlock(&audio->mutex);
    if (audio->capture_thread_started) {
        pthread_join(audio->capture_thread, NULL);
        audio->capture_thread_started = 0;
    }
    if (audio->playback_thread_started) {
        pthread_join(audio->playback_thread, NULL);
        audio->playback_thread_started = 0;
    }
}

void ptt_pc_audio_destroy(ptt_pc_audio *audio)
{
    if (audio == NULL) {
        return;
    }
    ptt_pc_audio_stop(audio);
    ptt_audio_decoder_destroy(audio->decoder);
    ptt_audio_encoder_destroy(audio->encoder);
    pthread_cond_destroy(&audio->playback_condition);
    pthread_cond_destroy(&audio->capture_condition);
    pthread_mutex_destroy(&audio->mutex);
    free(audio);
}
