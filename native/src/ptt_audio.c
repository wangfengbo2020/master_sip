#include "ptt_audio.h"

#include <limits.h>
#include <stdlib.h>

#include <opus.h>

struct ptt_audio_encoder {
    OpusEncoder *opus;
    int16_t pcm8[PTT_AUDIO_FRAME_SAMPLES];
};

struct ptt_audio_decoder {
    OpusDecoder *opus;
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
