#ifndef PTT_AUDIO_H
#define PTT_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTT_CAPTURE_SAMPLE_RATE 48000u
#define PTT_AUDIO_SAMPLE_RATE 8000u
#define PTT_AUDIO_CHANNELS 1u
#define PTT_AUDIO_FRAME_MS 20u
#define PTT_CAPTURE_FRAME_SAMPLES 960u
#define PTT_AUDIO_FRAME_SAMPLES 160u
#define PTT_OPUS_MAX_PACKET_BYTES 400u
#define PTT_AUDIO_MAX_DECODE_SAMPLES 960u
#define PTT_AUDIO_START_BUFFER_MS 150u
#define PTT_AUDIO_QUEUE_TARGET_MS 1000u
#define PTT_AUDIO_QUEUE_HARD_LIMIT_MS 1500u
#define PTT_AUDIO_QUEUE_RETAIN_MS 750u
#define PTT_AUDIO_START_BUFFER_FRAMES \
    (PTT_AUDIO_START_BUFFER_MS / PTT_AUDIO_FRAME_MS)
#define PTT_AUDIO_QUEUE_TARGET_FRAMES \
    (PTT_AUDIO_QUEUE_TARGET_MS / PTT_AUDIO_FRAME_MS)
#define PTT_AUDIO_QUEUE_HARD_LIMIT_FRAMES \
    (PTT_AUDIO_QUEUE_HARD_LIMIT_MS / PTT_AUDIO_FRAME_MS)
#define PTT_AUDIO_QUEUE_RETAIN_FRAMES \
    (PTT_AUDIO_QUEUE_RETAIN_MS / PTT_AUDIO_FRAME_MS)

typedef struct ptt_audio_encoder ptt_audio_encoder;
typedef struct ptt_audio_decoder ptt_audio_decoder;

typedef struct {
    uint64_t received_packets;
    uint64_t normal_frames;
    uint64_t fec_frames;
    uint64_t plc_frames;
    uint64_t duplicate_packets;
    uint64_t late_packets;
    uint64_t reset_events;
    uint64_t catchup_dropped_frames;
    uint64_t overflow_dropped_packets;
    uint64_t overflow_trim_events;
    uint64_t decode_errors;
    size_t buffered_packets;
    size_t maximum_buffered_packets;
} ptt_audio_jitter_stats;

typedef enum {
    PTT_AUDIO_PLAYOUT_NONE = 0,
    PTT_AUDIO_PLAYOUT_NORMAL,
    PTT_AUDIO_PLAYOUT_FEC,
    PTT_AUDIO_PLAYOUT_PLC
} ptt_audio_playout_mode;

typedef struct {
    uint16_t sequence;
    ptt_audio_playout_mode mode;
    size_t buffered_packets;
} ptt_audio_playout_info;

ptt_audio_encoder *ptt_audio_encoder_create(unsigned int bitrate);
void ptt_audio_encoder_destroy(ptt_audio_encoder *encoder);

/* Encodes exactly one 20 ms, 48 kHz, PCM16 mono capture frame. */
int ptt_audio_encode_pcm48(ptt_audio_encoder *encoder,
                           const int16_t *pcm48,
                           size_t sample_count,
                           uint8_t *opus_packet,
                           size_t opus_capacity);

ptt_audio_decoder *ptt_audio_decoder_create(void);
void ptt_audio_decoder_destroy(ptt_audio_decoder *decoder);

/* Returns decoded 8 kHz mono samples, or a negative value on error. */
int ptt_audio_decode_opus(ptt_audio_decoder *decoder,
                          const uint8_t *opus_packet,
                          size_t opus_packet_length,
                          int16_t *pcm8,
                          size_t pcm_capacity_samples);

/*
 * Network-side jitter-buffer input. This function only stores a sequenced
 * RTP/Opus packet; it never decodes audio. It returns one when a packet was
 * accepted, zero for a duplicate/late packet, or a negative value on error.
 * The caller must serialize push/pull calls for a decoder instance.
 */
int ptt_audio_jitter_push(ptt_audio_decoder *decoder,
                          const uint8_t *opus_packet,
                          size_t opus_packet_length,
                          uint16_t sequence,
                          int marker);

/*
 * Playout-side jitter-buffer output. Call exactly once every 20 ms from an
 * independent playout clock. Playback starts after 7 packets (about 140 ms;
 * the configured 150 ms is rounded down to whole 20 ms frames). The receive
 * queue has a 1-second operating target and a 1.5-second hard limit. On hard
 * overflow, old packets are discarded in one operation and the newest 740 ms
 * (37 complete frames from the configured 750 ms) are retained. Normal
 * playout is always FIFO by RTP
 * sequence.
 * Missing frames are recovered with Opus FEC when possible and PLC otherwise.
 * It returns one 20 ms PCM frame, zero while inactive/prebuffering, or a
 * negative value on error.
 */
int ptt_audio_jitter_pull(ptt_audio_decoder *decoder,
                          int16_t *pcm8,
                          size_t pcm_capacity_samples);

/* Extended playout API that also identifies the RTP frame represented by PCM. */
int ptt_audio_jitter_pull_ex(ptt_audio_decoder *decoder,
                             int16_t *pcm8,
                             size_t pcm_capacity_samples,
                             ptt_audio_playout_info *info);

const char *ptt_audio_playout_mode_name(ptt_audio_playout_mode mode);

/* Compatibility helper. New real-time clients should use push/pull above. */
int ptt_audio_decode_rtp_opus(ptt_audio_decoder *decoder,
                              const uint8_t *opus_packet,
                              size_t opus_packet_length,
                              uint16_t sequence,
                              int marker,
                              int16_t *pcm8,
                              size_t pcm_capacity_samples);

void ptt_audio_decoder_reset_jitter(ptt_audio_decoder *decoder);
void ptt_audio_decoder_get_jitter_stats(ptt_audio_decoder *decoder,
                                        ptt_audio_jitter_stats *stats,
                                        int reset_interval);

#ifdef __cplusplus
}
#endif

#endif
