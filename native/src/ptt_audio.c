#include "ptt_audio.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <opus.h>

#define PTT_JITTER_CAPACITY_PACKETS 160u
#define PTT_JITTER_START_PACKETS PTT_AUDIO_START_BUFFER_FRAMES
#define PTT_JITTER_HARD_LIMIT_PACKETS PTT_AUDIO_QUEUE_HARD_LIMIT_FRAMES
#define PTT_JITTER_RETAIN_PACKETS PTT_AUDIO_QUEUE_RETAIN_FRAMES
#define PTT_JITTER_MAX_MISSING_FRAMES 10u

typedef struct {
    int valid;
    uint16_t sequence;
    size_t length;
    uint8_t payload[PTT_OPUS_MAX_PACKET_BYTES];
} ptt_jitter_slot;

struct ptt_audio_encoder {
    OpusEncoder *opus;
    int16_t pcm8[PTT_AUDIO_FRAME_SAMPLES];
};

struct ptt_audio_decoder {
    OpusDecoder *opus;
    ptt_jitter_slot jitter[PTT_JITTER_CAPACITY_PACKETS];
    uint16_t expected_sequence;
    size_t jitter_count;
    int jitter_initialized;
    int jitter_primed;
    unsigned int consecutive_missing_frames;
    ptt_audio_jitter_stats jitter_stats;
};

static int apply_encoder_settings(OpusEncoder *encoder, unsigned int bitrate)
{
    if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE((int)bitrate)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(3)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_DTX(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK ||
        opus_encoder_ctl(encoder,
                         OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND)) != OPUS_OK) {
        return -1;
    }
    return 0;
}

ptt_audio_encoder *ptt_audio_encoder_create(unsigned int bitrate)
{
    ptt_audio_encoder *encoder;
    int error = OPUS_OK;

    if (bitrate < 6000u || bitrate > 128000u) {
        return NULL;
    }
    encoder = (ptt_audio_encoder *)calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        return NULL;
    }
    encoder->opus = opus_encoder_create((opus_int32)PTT_AUDIO_SAMPLE_RATE,
                                        (int)PTT_AUDIO_CHANNELS,
                                        OPUS_APPLICATION_VOIP,
                                        &error);
    if (encoder->opus == NULL || error != OPUS_OK ||
        apply_encoder_settings(encoder->opus, bitrate) != 0) {
        if (encoder->opus != NULL) {
            opus_encoder_destroy(encoder->opus);
        }
        free(encoder);
        return NULL;
    }
    return encoder;
}

void ptt_audio_encoder_destroy(ptt_audio_encoder *encoder)
{
    if (encoder == NULL) {
        return;
    }
    if (encoder->opus != NULL) {
        opus_encoder_destroy(encoder->opus);
    }
    free(encoder);
}

int ptt_audio_encode_pcm48(ptt_audio_encoder *encoder,
                           const int16_t *pcm48,
                           size_t sample_count,
                           uint8_t *opus_packet,
                           size_t opus_capacity)
{
    size_t output_index;

    if (encoder == NULL || encoder->opus == NULL || pcm48 == NULL ||
        sample_count != PTT_CAPTURE_FRAME_SAMPLES || opus_packet == NULL ||
        opus_capacity == 0 || opus_capacity > INT_MAX) {
        return -1;
    }

    /*
     * Low-cost 6:1 downsampler. Averaging each six-sample group provides a
     * simple anti-aliasing low-pass filter suitable for the narrowband voice
     * prototype while keeping the future embedded implementation small.
     */
    for (output_index = 0; output_index < PTT_AUDIO_FRAME_SAMPLES; ++output_index) {
        size_t input_index = output_index * 6u;
        int32_t sum = (int32_t)pcm48[input_index] +
                      (int32_t)pcm48[input_index + 1u] +
                      (int32_t)pcm48[input_index + 2u] +
                      (int32_t)pcm48[input_index + 3u] +
                      (int32_t)pcm48[input_index + 4u] +
                      (int32_t)pcm48[input_index + 5u];
        encoder->pcm8[output_index] = (int16_t)(sum / 6);
    }

    return opus_encode(encoder->opus,
                       encoder->pcm8,
                       (int)PTT_AUDIO_FRAME_SAMPLES,
                       opus_packet,
                       (opus_int32)opus_capacity);
}

ptt_audio_decoder *ptt_audio_decoder_create(void)
{
    ptt_audio_decoder *decoder;
    int error = OPUS_OK;

    decoder = (ptt_audio_decoder *)calloc(1, sizeof(*decoder));
    if (decoder == NULL) {
        return NULL;
    }
    decoder->opus = opus_decoder_create((opus_int32)PTT_AUDIO_SAMPLE_RATE,
                                        (int)PTT_AUDIO_CHANNELS,
                                        &error);
    if (decoder->opus == NULL || error != OPUS_OK) {
        if (decoder->opus != NULL) {
            opus_decoder_destroy(decoder->opus);
        }
        free(decoder);
        return NULL;
    }
    return decoder;
}

void ptt_audio_decoder_destroy(ptt_audio_decoder *decoder)
{
    if (decoder == NULL) {
        return;
    }
    if (decoder->opus != NULL) {
        opus_decoder_destroy(decoder->opus);
    }
    free(decoder);
}

int ptt_audio_decode_opus(ptt_audio_decoder *decoder,
                          const uint8_t *opus_packet,
                          size_t opus_packet_length,
                          int16_t *pcm8,
                          size_t pcm_capacity_samples)
{
    if (decoder == NULL || decoder->opus == NULL || opus_packet == NULL ||
        opus_packet_length == 0 || opus_packet_length > INT_MAX || pcm8 == NULL ||
        pcm_capacity_samples < PTT_AUDIO_FRAME_SAMPLES ||
        pcm_capacity_samples > INT_MAX) {
        return -1;
    }
    return opus_decode(decoder->opus,
                       opus_packet,
                       (opus_int32)opus_packet_length,
                       pcm8,
                       (int)pcm_capacity_samples,
                       0);
}

static void reset_jitter_state(ptt_audio_decoder *decoder,
                               uint16_t expected_sequence,
                               int initialize,
                               int count_event)
{
    memset(decoder->jitter, 0, sizeof(decoder->jitter));
    decoder->expected_sequence = expected_sequence;
    decoder->jitter_count = 0;
    decoder->jitter_initialized = initialize;
    decoder->jitter_primed = 0;
    decoder->consecutive_missing_frames = 0;
    if (count_event) {
        ++decoder->jitter_stats.reset_events;
    }
    if (decoder->opus != NULL) {
        (void)opus_decoder_ctl(decoder->opus, OPUS_RESET_STATE);
    }
}

static ptt_jitter_slot *find_jitter_slot(ptt_audio_decoder *decoder,
                                         uint16_t sequence)
{
    ptt_jitter_slot *slot =
        &decoder->jitter[(size_t)sequence % PTT_JITTER_CAPACITY_PACKETS];

    return slot->valid && slot->sequence == sequence ? slot : NULL;
}

static void remove_jitter_slot(ptt_audio_decoder *decoder,
                               ptt_jitter_slot *slot)
{
    if (slot == NULL || !slot->valid) {
        return;
    }
    slot->valid = 0;
    slot->length = 0;
    if (decoder->jitter_count > 0) {
        --decoder->jitter_count;
    }
}

static int decode_plc(ptt_audio_decoder *decoder,
                      int16_t *pcm8,
                      size_t pcm_capacity_samples)
{
    (void)pcm_capacity_samples;
    return opus_decode(decoder->opus,
                       NULL,
                       0,
                       pcm8,
                       (int)PTT_AUDIO_FRAME_SAMPLES,
                       0);
}

/*
 * A normal queue is played strictly from the oldest RTP sequence. If an
 * arriving packet would put the sequence span beyond 1.5 seconds, advance
 * the FIFO head so roughly 740 ms of older audio is retained. This intentionally trades
 * old speech for bounded delay while retaining the newest contiguous phrase.
 */
static void trim_jitter_on_hard_overflow(ptt_audio_decoder *decoder,
                                         uint16_t incoming_sequence)
{
    size_t index;
    size_t removed_packets = 0;
    uint16_t new_expected =
        (uint16_t)(incoming_sequence - PTT_JITTER_RETAIN_PACKETS);
    uint16_t skipped_frames =
        (uint16_t)(new_expected - decoder->expected_sequence);

    for (index = 0; index < PTT_JITTER_CAPACITY_PACKETS; ++index) {
        ptt_jitter_slot *candidate = &decoder->jitter[index];

        if (!candidate->valid) {
            continue;
        }
        if ((int16_t)(candidate->sequence - new_expected) < 0) {
            remove_jitter_slot(decoder, candidate);
            ++removed_packets;
        }
    }
    decoder->expected_sequence = new_expected;
    decoder->consecutive_missing_frames = 0;
    decoder->jitter_primed =
        decoder->jitter_count >= PTT_JITTER_START_PACKETS;
    decoder->jitter_stats.catchup_dropped_frames += skipped_frames;
    decoder->jitter_stats.overflow_dropped_packets += removed_packets;
    ++decoder->jitter_stats.overflow_trim_events;
}

int ptt_audio_jitter_push(ptt_audio_decoder *decoder,
                          const uint8_t *opus_packet,
                          size_t opus_packet_length,
                          uint16_t sequence,
                          int marker)
{
    ptt_jitter_slot *slot;
    int16_t sequence_delta;

    if (decoder == NULL || decoder->opus == NULL || opus_packet == NULL ||
        opus_packet_length == 0 ||
        opus_packet_length > PTT_OPUS_MAX_PACKET_BYTES) {
        return -1;
    }

    ++decoder->jitter_stats.received_packets;
    if (marker) {
        reset_jitter_state(decoder, sequence, 1, 1);
    } else if (!decoder->jitter_initialized) {
        reset_jitter_state(decoder, sequence, 1, 0);
    }

    sequence_delta = (int16_t)(sequence - decoder->expected_sequence);
    if (sequence_delta < 0) {
        ++decoder->jitter_stats.late_packets;
        return 0;
    }
    if ((uint16_t)sequence_delta >= PTT_JITTER_HARD_LIMIT_PACKETS) {
        trim_jitter_on_hard_overflow(decoder, sequence);
        sequence_delta = (int16_t)(sequence - decoder->expected_sequence);
    }
    if ((uint16_t)sequence_delta >= PTT_JITTER_CAPACITY_PACKETS) {
        ++decoder->jitter_stats.overflow_dropped_packets;
        return 0;
    }

    slot = &decoder->jitter[(size_t)sequence % PTT_JITTER_CAPACITY_PACKETS];
    if (slot->valid && slot->sequence == sequence) {
        ++decoder->jitter_stats.duplicate_packets;
        return 0;
    }
    if (slot->valid) {
        ++decoder->jitter_stats.overflow_dropped_packets;
        return 0;
    }
    slot->valid = 1;
    slot->sequence = sequence;
    slot->length = opus_packet_length;
    memcpy(slot->payload, opus_packet, opus_packet_length);
    ++decoder->jitter_count;
    if (decoder->jitter_count > decoder->jitter_stats.maximum_buffered_packets) {
        decoder->jitter_stats.maximum_buffered_packets = decoder->jitter_count;
    }

    if (!decoder->jitter_primed) {
        if (decoder->jitter_count >= PTT_JITTER_START_PACKETS) {
            decoder->jitter_primed = 1;
        }
    }

    return 1;
}

int ptt_audio_jitter_pull_ex(ptt_audio_decoder *decoder,
                             int16_t *pcm8,
                             size_t pcm_capacity_samples,
                             ptt_audio_playout_info *info)
{
    ptt_jitter_slot *slot;
    ptt_jitter_slot *next_slot;
    int samples;
    uint16_t playout_sequence;

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        info->mode = PTT_AUDIO_PLAYOUT_NONE;
    }

    if (decoder == NULL || decoder->opus == NULL || pcm8 == NULL ||
        pcm_capacity_samples < PTT_AUDIO_FRAME_SAMPLES ||
        pcm_capacity_samples > INT_MAX) {
        return -1;
    }
    if (!decoder->jitter_initialized || !decoder->jitter_primed) {
        return 0;
    }
    if (decoder->jitter_count == 0) {
        reset_jitter_state(decoder, 0, 0, 1);
        return 0;
    }

    playout_sequence = decoder->expected_sequence;
    if (info != NULL) {
        info->sequence = playout_sequence;
    }
    slot = find_jitter_slot(decoder, playout_sequence);
    if (slot != NULL) {
        samples = opus_decode(decoder->opus,
                              slot->payload,
                              (opus_int32)slot->length,
                              pcm8,
                              (int)PTT_AUDIO_FRAME_SAMPLES,
                              0);
        remove_jitter_slot(decoder, slot);
        if (samples > 0) {
            ++decoder->jitter_stats.normal_frames;
            decoder->consecutive_missing_frames = 0;
            if (info != NULL) {
                info->mode = PTT_AUDIO_PLAYOUT_NORMAL;
            }
        } else {
            ++decoder->jitter_stats.decode_errors;
            samples = decode_plc(decoder, pcm8, pcm_capacity_samples);
            if (samples > 0) {
                ++decoder->jitter_stats.plc_frames;
                if (info != NULL) {
                    info->mode = PTT_AUDIO_PLAYOUT_PLC;
                }
            }
        }
    } else {
        ++decoder->consecutive_missing_frames;
        next_slot = find_jitter_slot(decoder,
                                     (uint16_t)(decoder->expected_sequence + 1u));
        if (next_slot != NULL) {
            samples = opus_decode(decoder->opus,
                                  next_slot->payload,
                                  (opus_int32)next_slot->length,
                                  pcm8,
                                  (int)PTT_AUDIO_FRAME_SAMPLES,
                                  1);
            if (samples > 0) {
                ++decoder->jitter_stats.fec_frames;
                if (info != NULL) {
                    info->mode = PTT_AUDIO_PLAYOUT_FEC;
                }
            } else {
                ++decoder->jitter_stats.decode_errors;
                samples = decode_plc(decoder, pcm8, pcm_capacity_samples);
                if (samples > 0) {
                    ++decoder->jitter_stats.plc_frames;
                    if (info != NULL) {
                        info->mode = PTT_AUDIO_PLAYOUT_PLC;
                    }
                }
            }
        } else {
            samples = decode_plc(decoder, pcm8, pcm_capacity_samples);
            if (samples > 0) {
                ++decoder->jitter_stats.plc_frames;
                if (info != NULL) {
                    info->mode = PTT_AUDIO_PLAYOUT_PLC;
                }
            } else {
                ++decoder->jitter_stats.decode_errors;
            }
        }
    }
    ++decoder->expected_sequence;
    if (decoder->jitter_count == 0 &&
        decoder->consecutive_missing_frames >= PTT_JITTER_MAX_MISSING_FRAMES) {
        reset_jitter_state(decoder, 0, 0, 1);
    }
    if (info != NULL) {
        info->buffered_packets = decoder->jitter_count;
    }
    return samples;
}

int ptt_audio_jitter_pull(ptt_audio_decoder *decoder,
                          int16_t *pcm8,
                          size_t pcm_capacity_samples)
{
    return ptt_audio_jitter_pull_ex(decoder,
                                    pcm8,
                                    pcm_capacity_samples,
                                    NULL);
}

const char *ptt_audio_playout_mode_name(ptt_audio_playout_mode mode)
{
    switch (mode) {
    case PTT_AUDIO_PLAYOUT_NORMAL: return "normal";
    case PTT_AUDIO_PLAYOUT_FEC: return "fec";
    case PTT_AUDIO_PLAYOUT_PLC: return "plc";
    case PTT_AUDIO_PLAYOUT_NONE:
    default: return "none";
    }
}

int ptt_audio_decode_rtp_opus(ptt_audio_decoder *decoder,
                              const uint8_t *opus_packet,
                              size_t opus_packet_length,
                              uint16_t sequence,
                              int marker,
                              int16_t *pcm8,
                              size_t pcm_capacity_samples)
{
    int pushed = ptt_audio_jitter_push(decoder,
                                       opus_packet,
                                       opus_packet_length,
                                       sequence,
                                       marker);

    if (pushed < 0) {
        return pushed;
    }
    return ptt_audio_jitter_pull(decoder, pcm8, pcm_capacity_samples);
}

void ptt_audio_decoder_reset_jitter(ptt_audio_decoder *decoder)
{
    if (decoder == NULL) {
        return;
    }
    reset_jitter_state(decoder, 0, 0, 1);
}

void ptt_audio_decoder_get_jitter_stats(ptt_audio_decoder *decoder,
                                        ptt_audio_jitter_stats *stats,
                                        int reset_interval)
{
    if (decoder == NULL || stats == NULL) {
        return;
    }
    *stats = decoder->jitter_stats;
    stats->buffered_packets = decoder->jitter_count;
    if (reset_interval) {
        memset(&decoder->jitter_stats, 0, sizeof(decoder->jitter_stats));
        decoder->jitter_stats.maximum_buffered_packets = decoder->jitter_count;
    }
}
