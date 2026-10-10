#ifndef PTT_COMMON_H
#define PTT_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define PTT_MAX_SIP_PACKET 4096
#define PTT_MAX_RTP_PACKET 1500
#define PTT_MAX_ID 64
#define PTT_MAX_GROUP 64
#define PTT_OPUS_PAYLOAD_TYPE 111
#define PTT_RTP_KEEPALIVE_PAYLOAD_TYPE 126
#define PTT_RTP_KEEPALIVE_INTERVAL_MS 10000u
#define PTT_OPUS_SAMPLE_RATE 48000u
#define PTT_OPUS_PACKETS_PER_SECOND 50u
#define PTT_OPUS_PACKET_MS 20u
#define PTT_OPUS_TIMESTAMP_STEP 960u
#define PTT_RTP_SEND_TIME_PROFILE 0x5054u
#define PTT_RTP_HEADER_WITH_SEND_TIME 24u

typedef struct {
    uint8_t payload_type;
    uint8_t marker;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
    int has_send_time_ms;
    uint64_t send_time_ms;
    size_t header_length;
    const uint8_t *payload;
    size_t payload_length;
} ptt_rtp_view;

uint64_t ptt_now_ms(void);
uint64_t ptt_unix_time_ms(void);

int ptt_get_header(const char *message,
                   const char *header_name,
                   char *output,
                   size_t output_size);

const char *ptt_get_body(const char *message);

int ptt_parse_cseq(const char *message,
                   unsigned int *number,
                   char *method,
                   size_t method_size);

uint32_t ptt_hash_text(const char *text);

size_t ptt_build_rtp(uint8_t *packet,
                     size_t packet_capacity,
                     uint8_t payload_type,
                     uint8_t marker,
                     uint16_t sequence,
                     uint32_t timestamp,
                     uint32_t ssrc,
                     uint64_t send_time_ms,
                     const uint8_t *payload,
                     size_t payload_length);

int ptt_parse_rtp(const uint8_t *packet,
                  size_t packet_length,
                  ptt_rtp_view *view);

#endif

