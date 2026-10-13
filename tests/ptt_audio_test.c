#include "ptt_audio.h"
#include "ptt_common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int encode_test_frame(ptt_audio_encoder *encoder,
                             uint8_t *packet,
                             size_t packet_capacity,
                             unsigned int phase)
{
    int16_t capture[PTT_CAPTURE_FRAME_SAMPLES];
    size_t index;

    for (index = 0; index < PTT_CAPTURE_FRAME_SAMPLES; ++index) {
        capture[index] = ((index / 48u + phase) % 2u) == 0u ? 9000 : -9000;
    }
    return ptt_audio_encode_pcm48(encoder,
                                  capture,
                                  PTT_CAPTURE_FRAME_SAMPLES,
                                  packet,
                                  packet_capacity);
}

int main(void)
{
    ptt_audio_encoder *encoder = ptt_audio_encoder_create(16000u);
    ptt_audio_decoder *decoder = ptt_audio_decoder_create();
    ptt_audio_decoder *ordered_decoder = ptt_audio_decoder_create();
    ptt_audio_decoder *fec_decoder = ptt_audio_decoder_create();
    ptt_audio_decoder *overflow_decoder = ptt_audio_decoder_create();
    int16_t playback[PTT_AUDIO_MAX_DECODE_SAMPLES];
    uint8_t opus[PTT_OPUS_MAX_PACKET_BYTES];
    uint8_t rtp[PTT_MAX_RTP_PACKET];
    ptt_audio_playout_info playout;
    ptt_audio_jitter_stats stats;
    ptt_rtp_view view;
    int encoded;
    int decoded;
    unsigned int index;
    size_t rtp_length;
    const uint16_t ordered_base_sequence = 100u;
    const uint16_t overflow_base_sequence = 300u;
    const uint32_t source_frame_id = 123456u;
    const uint64_t capture_time_ms = UINT64_C(1770000000123);
    const uint64_t send_time_ms = UINT64_C(1770000000130);

    if (encoder == NULL || decoder == NULL || ordered_decoder == NULL ||
        fec_decoder == NULL || overflow_decoder == NULL) {
        fprintf(stderr, "cannot create Opus codec\n");
        return 1;
    }
    encoded = encode_test_frame(encoder, opus, sizeof(opus), 0u);
    if (encoded <= 0) {
        fprintf(stderr, "Opus encode failed: %d\n", encoded);
        return 2;
    }
    decoded = ptt_audio_decode_opus(decoder,
                                    opus,
                                    (size_t)encoded,
                                    playback,
                                    PTT_AUDIO_MAX_DECODE_SAMPLES);
    if (decoded != (int)PTT_AUDIO_FRAME_SAMPLES) {
        fprintf(stderr, "unexpected decoded samples: %d\n", decoded);
        return 3;
    }

    /*
     * Insert one fewer than the configured startup frame count. The oldest
     * packet arrives first (and carries the marker), while the rest arrive in
     * reverse order with one sequence omitted. Playout must wait for the last
     * startup packet and then follow RTP sequence order, not socket order.
     */
    if (ptt_audio_jitter_push(ordered_decoder,
                              opus,
                              (size_t)encoded,
                              ordered_base_sequence,
                              1) != 1) {
        fprintf(stderr, "cannot add first startup frame\n");
        return 4;
    }
    for (index = PTT_AUDIO_START_BUFFER_FRAMES - 1u; index > 0u; --index) {
        if (index == PTT_AUDIO_START_BUFFER_FRAMES - 2u) {
            continue;
        }
        if (ptt_audio_jitter_push(ordered_decoder,
                                  opus,
                                  (size_t)encoded,
                                  (uint16_t)(ordered_base_sequence + index),
                                  0) != 1) {
            fprintf(stderr, "cannot fill startup prebuffer at %u\n", index);
            return 4;
        }
    }
    if (ptt_audio_jitter_pull_ex(ordered_decoder,
                                 playback,
                                 PTT_AUDIO_MAX_DECODE_SAMPLES,
                                 &playout) != 0) {
        fprintf(stderr,
                "jitter buffer started before %u frames\n",
                PTT_AUDIO_START_BUFFER_FRAMES);
        return 5;
    }
    if (ptt_audio_jitter_push(ordered_decoder,
                              opus,
                              (size_t)encoded,
                              (uint16_t)(ordered_base_sequence +
                                         PTT_AUDIO_START_BUFFER_FRAMES - 2u),
                              0) != 1) {
        fprintf(stderr, "cannot add final startup frame\n");
        return 6;
    }
    decoded = ptt_audio_jitter_pull_ex(ordered_decoder,
                                       playback,
                                       PTT_AUDIO_MAX_DECODE_SAMPLES,
                                       &playout);
    if (decoded != (int)PTT_AUDIO_FRAME_SAMPLES ||
        playout.sequence != ordered_base_sequence ||
        playout.mode != PTT_AUDIO_PLAYOUT_NORMAL) {
        fprintf(stderr,
                "FIFO did not play oldest frame first: decoded=%d seq=%u mode=%s\n",
                decoded,
                playout.sequence,
                ptt_audio_playout_mode_name(playout.mode));
        return 7;
    }
    decoded = ptt_audio_jitter_pull_ex(ordered_decoder,
                                       playback,
                                       PTT_AUDIO_MAX_DECODE_SAMPLES,
                                       &playout);
    if (decoded != (int)PTT_AUDIO_FRAME_SAMPLES ||
        playout.sequence != (uint16_t)(ordered_base_sequence + 1u)) {
        fprintf(stderr, "unexpected second FIFO frame: seq=%u\n", playout.sequence);
        return 8;
    }

    /* Prime around one missing packet and verify FEC/PLC represents that sequence. */
    for (index = 0; index < 16u; ++index) {
        uint16_t sequence = (uint16_t)(200u + index);
        if (sequence == 202u) {
            continue;
        }
        if (ptt_audio_jitter_push(fec_decoder,
                                  opus,
                                  (size_t)encoded,
                                  sequence,
                                  sequence == 200u) != 1) {
            fprintf(stderr, "cannot prime FEC test at seq=%u\n", sequence);
            return 9;
        }
    }
    (void)ptt_audio_jitter_pull_ex(fec_decoder,
                                   playback,
                                   PTT_AUDIO_MAX_DECODE_SAMPLES,
                                   &playout);
    (void)ptt_audio_jitter_pull_ex(fec_decoder,
                                   playback,
                                   PTT_AUDIO_MAX_DECODE_SAMPLES,
                                   &playout);
    decoded = ptt_audio_jitter_pull_ex(fec_decoder,
                                       playback,
                                       PTT_AUDIO_MAX_DECODE_SAMPLES,
                                       &playout);
    if (decoded != (int)PTT_AUDIO_FRAME_SAMPLES ||
        playout.sequence != 202u ||
        (playout.mode != PTT_AUDIO_PLAYOUT_FEC &&
         playout.mode != PTT_AUDIO_PLAYOUT_PLC)) {
        fprintf(stderr,
                "missing frame concealment failed: seq=%u mode=%s\n",
                playout.sequence,
                ptt_audio_playout_mode_name(playout.mode));
        return 10;
    }

    /*
     * Fill the configured hard limit. The next frame trims the FIFO head,
     * retaining the configured recent window plus the new arrival.
     */
    for (index = 0; index < PTT_AUDIO_QUEUE_HARD_LIMIT_FRAMES; ++index) {
        if (ptt_audio_jitter_push(overflow_decoder,
                                  opus,
                                  (size_t)encoded,
                                  (uint16_t)(overflow_base_sequence + index),
                                  index == 0u) != 1) {
            fprintf(stderr, "hard-limit queue rejected frame %u\n", index);
            return 11;
        }
    }
    if (ptt_audio_jitter_push(overflow_decoder,
                              opus,
                              (size_t)encoded,
                              (uint16_t)(overflow_base_sequence +
                                         PTT_AUDIO_QUEUE_HARD_LIMIT_FRAMES),
                              0) != 1) {
        fprintf(stderr, "hard-overflow packet was not accepted after trimming\n");
        return 12;
    }
    ptt_audio_decoder_get_jitter_stats(overflow_decoder, &stats, 0);
    if (stats.buffered_packets != PTT_AUDIO_QUEUE_RETAIN_FRAMES + 1u ||
        stats.overflow_dropped_packets !=
            PTT_AUDIO_QUEUE_HARD_LIMIT_FRAMES -
                PTT_AUDIO_QUEUE_RETAIN_FRAMES ||
        stats.catchup_dropped_frames !=
            PTT_AUDIO_QUEUE_HARD_LIMIT_FRAMES -
                PTT_AUDIO_QUEUE_RETAIN_FRAMES ||
        stats.overflow_trim_events != 1u) {
        fprintf(stderr,
                "unexpected overflow stats: buffered=%zu overflow=%llu "
                "old_drop=%llu trims=%llu\n",
                stats.buffered_packets,
                (unsigned long long)stats.overflow_dropped_packets,
                (unsigned long long)stats.catchup_dropped_frames,
                (unsigned long long)stats.overflow_trim_events);
        return 13;
    }
    decoded = ptt_audio_jitter_pull_ex(overflow_decoder,
                                       playback,
                                       PTT_AUDIO_MAX_DECODE_SAMPLES,
                                       &playout);
    if (decoded != (int)PTT_AUDIO_FRAME_SAMPLES ||
        playout.sequence !=
            (uint16_t)(overflow_base_sequence +
                       PTT_AUDIO_QUEUE_HARD_LIMIT_FRAMES -
                       PTT_AUDIO_QUEUE_RETAIN_FRAMES)) {
        fprintf(stderr,
                "overflow discarded old FIFO head: decoded=%d seq=%u\n",
                decoded,
                playout.sequence);
        return 14;
    }

    rtp_length = ptt_build_rtp_with_frame_metadata(rtp,
                                                    sizeof(rtp),
                                                    PTT_OPUS_PAYLOAD_TYPE,
                                                    1u,
                                                    456u,
                                                    960u,
                                                    0x11223344u,
                                                    send_time_ms,
                                                    source_frame_id,
                                                    capture_time_ms,
                                                    opus,
                                                    (size_t)encoded);
    memset(&view, 0, sizeof(view));
    if (rtp_length == 0 || !ptt_parse_rtp(rtp, rtp_length, &view) ||
        !view.has_send_time_ms || view.send_time_ms != send_time_ms ||
        !view.has_source_frame_id || view.source_frame_id != source_frame_id ||
        !view.has_capture_time_ms || view.capture_time_ms != capture_time_ms ||
        view.header_length != PTT_RTP_HEADER_WITH_FRAME_METADATA ||
        view.payload_length != (size_t)encoded ||
        memcmp(view.payload, opus, (size_t)encoded) != 0) {
        fprintf(stderr, "RTP frame metadata round-trip failed\n");
        return 15;
    }

    printf("codec ok: encoded=%d bytes decoded=%d samples\n", encoded, decoded);
    printf("jitter ok: start=%u ms (%u frames) target=%u ms hard=%u ms "
           "retain=%u ms FIFO_head=%u overflow_trim_old=%llu\n",
           PTT_AUDIO_START_BUFFER_MS,
           PTT_AUDIO_START_BUFFER_FRAMES,
           PTT_AUDIO_QUEUE_TARGET_MS,
           PTT_AUDIO_QUEUE_HARD_LIMIT_MS,
           PTT_AUDIO_QUEUE_RETAIN_MS,
           playout.sequence,
           (unsigned long long)stats.overflow_dropped_packets);
    printf("metadata ok: capture_frame=%u capture_ms=%llu send_ms=%llu\n",
           view.source_frame_id,
           (unsigned long long)view.capture_time_ms,
           (unsigned long long)view.send_time_ms);

    ptt_audio_decoder_destroy(overflow_decoder);
    ptt_audio_decoder_destroy(fec_decoder);
    ptt_audio_decoder_destroy(ordered_decoder);
    ptt_audio_decoder_destroy(decoder);
    ptt_audio_encoder_destroy(encoder);
    return 0;
}
