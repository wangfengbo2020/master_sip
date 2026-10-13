#include "ptt_common.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

uint64_t ptt_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t ptt_unix_time_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void copy_trimmed(const char *begin,
                         const char *end,
                         char *output,
                         size_t output_size)
{
    size_t length;

    while (begin < end && isspace((unsigned char)*begin)) {
        ++begin;
    }
    while (end > begin && isspace((unsigned char)end[-1])) {
        --end;
    }

    length = (size_t)(end - begin);
    if (length >= output_size) {
        length = output_size - 1;
    }

    memcpy(output, begin, length);
    output[length] = '\0';
}

int ptt_get_header(const char *message,
                   const char *header_name,
                   char *output,
                   size_t output_size)
{
    const char *line;
    size_t name_length;

    if (message == NULL || header_name == NULL || output == NULL || output_size == 0) {
        return 0;
    }

    output[0] = '\0';
    name_length = strlen(header_name);
    line = strchr(message, '\n');
    if (line == NULL) {
        return 0;
    }
    ++line;

    while (*line != '\0') {
        const char *line_end = strchr(line, '\n');
        const char *content_end = line_end != NULL ? line_end : line + strlen(line);

        if (content_end > line && content_end[-1] == '\r') {
            --content_end;
        }
        if (content_end == line) {
            break;
        }

        if ((size_t)(content_end - line) > name_length &&
            strncasecmp(line, header_name, name_length) == 0 &&
            line[name_length] == ':') {
            copy_trimmed(line + name_length + 1, content_end, output, output_size);
            return 1;
        }

        if (line_end == NULL) {
            break;
        }
        line = line_end + 1;
    }

    return 0;
}

const char *ptt_get_body(const char *message)
{
    const char *body;

    if (message == NULL) {
        return NULL;
    }

    body = strstr(message, "\r\n\r\n");
    if (body != NULL) {
        return body + 4;
    }

    body = strstr(message, "\n\n");
    return body != NULL ? body + 2 : NULL;
}

int ptt_parse_cseq(const char *message,
                   unsigned int *number,
                   char *method,
                   size_t method_size)
{
    char value[128];
    unsigned int parsed_number;
    char parsed_method[32];

    if (number == NULL || method == NULL || method_size == 0) {
        return 0;
    }

    if (!ptt_get_header(message, "CSeq", value, sizeof(value))) {
        return 0;
    }

    if (sscanf(value, "%u %31s", &parsed_number, parsed_method) != 2) {
        return 0;
    }

    *number = parsed_number;
    strncpy(method, parsed_method, method_size - 1);
    method[method_size - 1] = '\0';
    return 1;
}

uint32_t ptt_hash_text(const char *text)
{
    uint32_t hash = 2166136261u;

    if (text == NULL) {
        return hash;
    }

    while (*text != '\0') {
        hash ^= (uint8_t)*text++;
        hash *= 16777619u;
    }
    return hash;
}

size_t ptt_build_rtp(uint8_t *packet,
                     size_t packet_capacity,
                     uint8_t payload_type,
                     uint8_t marker,
                     uint16_t sequence,
                     uint32_t timestamp,
                     uint32_t ssrc,
                     uint64_t send_time_ms,
                     const uint8_t *payload,
                     size_t payload_length)
{
    size_t i;

    if (packet == NULL || payload == NULL ||
        packet_capacity < PTT_RTP_HEADER_WITH_SEND_TIME + payload_length) {
        return 0;
    }

    /* RTP V=2, X=1. The private extension carries an unsigned 64-bit Unix ms value. */
    packet[0] = 0x90;
    packet[1] = (uint8_t)((marker ? 0x80 : 0x00) | (payload_type & 0x7f));
    packet[2] = (uint8_t)(sequence >> 8);
    packet[3] = (uint8_t)sequence;
    packet[4] = (uint8_t)(timestamp >> 24);
    packet[5] = (uint8_t)(timestamp >> 16);
    packet[6] = (uint8_t)(timestamp >> 8);
    packet[7] = (uint8_t)timestamp;
    packet[8] = (uint8_t)(ssrc >> 24);
    packet[9] = (uint8_t)(ssrc >> 16);
    packet[10] = (uint8_t)(ssrc >> 8);
    packet[11] = (uint8_t)ssrc;
    packet[12] = (uint8_t)(PTT_RTP_SEND_TIME_PROFILE >> 8);
    packet[13] = (uint8_t)PTT_RTP_SEND_TIME_PROFILE;
    packet[14] = 0;
    packet[15] = 2; /* Extension data length in 32-bit words: 8 bytes. */
    for (i = 0; i < 8; ++i) {
        packet[16 + i] = (uint8_t)(send_time_ms >> (56u - (unsigned int)i * 8u));
    }
    memcpy(packet + PTT_RTP_HEADER_WITH_SEND_TIME, payload, payload_length);
    return PTT_RTP_HEADER_WITH_SEND_TIME + payload_length;
}

size_t ptt_build_rtp_with_frame_metadata(uint8_t *packet,
                                         size_t packet_capacity,
                                         uint8_t payload_type,
                                         uint8_t marker,
                                         uint16_t sequence,
                                         uint32_t timestamp,
                                         uint32_t ssrc,
                                         uint64_t send_time_ms,
                                         uint32_t source_frame_id,
                                         uint64_t capture_time_ms,
                                         const uint8_t *payload,
                                         size_t payload_length)
{
    size_t i;

    if (packet == NULL || payload == NULL ||
        packet_capacity < PTT_RTP_HEADER_WITH_FRAME_METADATA + payload_length) {
        return 0;
    }

    packet[0] = 0x90; /* RTP V=2, X=1. */
    packet[1] = (uint8_t)((marker ? 0x80 : 0x00) | (payload_type & 0x7f));
    packet[2] = (uint8_t)(sequence >> 8);
    packet[3] = (uint8_t)sequence;
    packet[4] = (uint8_t)(timestamp >> 24);
    packet[5] = (uint8_t)(timestamp >> 16);
    packet[6] = (uint8_t)(timestamp >> 8);
    packet[7] = (uint8_t)timestamp;
    packet[8] = (uint8_t)(ssrc >> 24);
    packet[9] = (uint8_t)(ssrc >> 16);
    packet[10] = (uint8_t)(ssrc >> 8);
    packet[11] = (uint8_t)ssrc;
    packet[12] = (uint8_t)(PTT_RTP_SEND_TIME_PROFILE >> 8);
    packet[13] = (uint8_t)PTT_RTP_SEND_TIME_PROFILE;
    packet[14] = 0;
    packet[15] = 5; /* 20 bytes: send time, source frame id, capture time. */
    for (i = 0; i < 8; ++i) {
        packet[16 + i] = (uint8_t)(send_time_ms >> (56u - (unsigned int)i * 8u));
    }
    packet[24] = (uint8_t)(source_frame_id >> 24);
    packet[25] = (uint8_t)(source_frame_id >> 16);
    packet[26] = (uint8_t)(source_frame_id >> 8);
    packet[27] = (uint8_t)source_frame_id;
    for (i = 0; i < 8; ++i) {
        packet[28 + i] =
            (uint8_t)(capture_time_ms >> (56u - (unsigned int)i * 8u));
    }
    memcpy(packet + PTT_RTP_HEADER_WITH_FRAME_METADATA, payload, payload_length);
    return PTT_RTP_HEADER_WITH_FRAME_METADATA + payload_length;
}

int ptt_parse_rtp(const uint8_t *packet,
                  size_t packet_length,
                  ptt_rtp_view *view)
{
    size_t header_length;
    uint8_t csrc_count;
    uint8_t has_extension;
    uint8_t padding_length = 0;

    if (packet == NULL || view == NULL || packet_length < 12 || (packet[0] >> 6) != 2) {
        return 0;
    }

    csrc_count = packet[0] & 0x0f;
    has_extension = packet[0] & 0x10;
    view->has_send_time_ms = 0;
    view->send_time_ms = 0;
    view->has_source_frame_id = 0;
    view->source_frame_id = 0;
    view->has_capture_time_ms = 0;
    view->capture_time_ms = 0;
    header_length = 12u + (size_t)csrc_count * 4u;
    if (packet_length < header_length) {
        return 0;
    }

    if (has_extension) {
        uint16_t extension_profile;
        uint16_t extension_words;
        size_t extension_bytes;
        size_t i;

        if (packet_length < header_length + 4) {
            return 0;
        }
        extension_profile = (uint16_t)((packet[header_length] << 8) |
                                       packet[header_length + 1]);
        extension_words = (uint16_t)((packet[header_length + 2] << 8) |
                                     packet[header_length + 3]);
        extension_bytes = (size_t)extension_words * 4u;
        if (extension_bytes > packet_length - header_length - 4u) {
            return 0;
        }
        if (extension_profile == PTT_RTP_SEND_TIME_PROFILE && extension_bytes >= 8u) {
            uint64_t send_time_ms = 0;

            for (i = 0; i < 8; ++i) {
                send_time_ms = (send_time_ms << 8) | packet[header_length + 4u + i];
            }
            view->has_send_time_ms = 1;
            view->send_time_ms = send_time_ms;
            if (extension_bytes >= 20u) {
                uint64_t capture_time_ms = 0;

                view->source_frame_id =
                    ((uint32_t)packet[header_length + 12u] << 24) |
                    ((uint32_t)packet[header_length + 13u] << 16) |
                    ((uint32_t)packet[header_length + 14u] << 8) |
                    (uint32_t)packet[header_length + 15u];
                for (i = 0; i < 8; ++i) {
                    capture_time_ms =
                        (capture_time_ms << 8) |
                        packet[header_length + 16u + i];
                }
                view->has_source_frame_id = 1;
                view->has_capture_time_ms = 1;
                view->capture_time_ms = capture_time_ms;
            }
        }
        header_length += 4u + extension_bytes;
    }

    if ((packet[0] & 0x20) != 0) {
        padding_length = packet[packet_length - 1];
        if (padding_length == 0 || packet_length < header_length + padding_length) {
            return 0;
        }
    }

    view->marker = (packet[1] >> 7) & 1;
    view->payload_type = packet[1] & 0x7f;
    view->sequence = (uint16_t)((packet[2] << 8) | packet[3]);
    view->timestamp = ((uint32_t)packet[4] << 24) |
                      ((uint32_t)packet[5] << 16) |
                      ((uint32_t)packet[6] << 8) |
                      (uint32_t)packet[7];
    view->ssrc = ((uint32_t)packet[8] << 24) |
                 ((uint32_t)packet[9] << 16) |
                 ((uint32_t)packet[10] << 8) |
                 (uint32_t)packet[11];
    view->header_length = header_length;
    view->payload = packet + header_length;
    view->payload_length = packet_length - header_length - padding_length;
    return 1;
}

