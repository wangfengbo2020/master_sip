#ifndef PTT_PC_AUDIO_H
#define PTT_PC_AUDIO_H

#include <stddef.h>
#include <stdint.h>

typedef struct ptt_pc_audio ptt_pc_audio;

ptt_pc_audio *ptt_pc_audio_create(unsigned int opus_bitrate);
int ptt_pc_audio_start(ptt_pc_audio *audio);
void ptt_pc_audio_set_talking(ptt_pc_audio *audio, int talking);

/* Returns 1 when a packet was copied, 0 when the queue is empty. */
int ptt_pc_audio_take_opus(ptt_pc_audio *audio,
                           uint8_t *packet,
                           size_t capacity,
                           size_t *packet_length,
                           uint32_t *source_frame_id,
                           uint64_t *capture_time_ms);

void ptt_pc_audio_receive_opus(void *user_data,
                               const uint8_t *payload,
                               size_t payload_length,
                               uint16_t sequence,
                               int marker);

void ptt_pc_audio_stop(ptt_pc_audio *audio);
void ptt_pc_audio_destroy(ptt_pc_audio *audio);

#endif
