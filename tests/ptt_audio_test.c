#include "ptt_audio.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    ptt_audio_encoder *encoder = ptt_audio_encoder_create(16000u);
    ptt_audio_decoder *decoder = ptt_audio_decoder_create();
    int16_t capture[PTT_CAPTURE_FRAME_SAMPLES];
    int16_t playback[PTT_AUDIO_MAX_DECODE_SAMPLES];
    uint8_t packet[PTT_OPUS_MAX_PACKET_BYTES];
    int encoded;
    int decoded;
    size_t i;
    int nonzero = 0;

    if (encoder == NULL || decoder == NULL) {
        fprintf(stderr, "cannot create Opus codec\n");
        return 1;
    }
    for (i = 0; i < PTT_CAPTURE_FRAME_SAMPLES; ++i) {
        capture[i] = ((i / 48u) % 2u) == 0u ? 10000 : -10000;
    }
    encoded = ptt_audio_encode_pcm48(encoder,
                                     capture,
                                     PTT_CAPTURE_FRAME_SAMPLES,
                                     packet,
                                     sizeof(packet));
    if (encoded <= 0 || encoded > (int)sizeof(packet)) {
        fprintf(stderr, "Opus encode failed: %d\n", encoded);
        return 2;
    }
    decoded = ptt_audio_decode_opus(decoder,
                                    packet,
                                    (size_t)encoded,
                                    playback,
                                    PTT_AUDIO_MAX_DECODE_SAMPLES);
    if (decoded != (int)PTT_AUDIO_FRAME_SAMPLES) {
        fprintf(stderr, "unexpected decoded samples: %d\n", decoded);
        return 3;
    }
    for (i = 0; i < (size_t)decoded; ++i) {
        if (playback[i] != 0) {
            nonzero = 1;
            break;
        }
    }
    if (!nonzero) {
        fprintf(stderr, "decoded frame is silent\n");
        return 4;
    }
    printf("codec ok: capture=%u samples encoded=%d bytes decoded=%d samples\n",
           PTT_CAPTURE_FRAME_SAMPLES,
           encoded,
           decoded);
    ptt_audio_decoder_destroy(decoder);
    ptt_audio_encoder_destroy(encoder);
    return 0;
}
