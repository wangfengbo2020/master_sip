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

typedef struct ptt_audio_encoder ptt_audio_encoder;
typedef struct ptt_audio_decoder ptt_audio_decoder;

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

#ifdef __cplusplus
}
#endif

#endif
