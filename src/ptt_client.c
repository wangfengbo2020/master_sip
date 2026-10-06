#include "ptt_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    const char *server_host;
    uint16_t server_sip_port;
    uint16_t server_rtp_port;
    uint16_t local_sip_port;
    uint16_t local_rtp_port;
    char id[PTT_MAX_ID];
    char group[PTT_MAX_GROUP];
    unsigned int run_seconds;
    unsigned int auto_talk_delay_ms;
    unsigned int auto_talk_ms;
    unsigned int opus_bitrate;
} client_config;

typedef struct {
    client_config config;
    int sip_fd;
    int rtp_fd;
    struct sockaddr_in server_sip_address;
    struct sockaddr_in server_rtp_address;
    char local_ip[INET_ADDRSTRLEN];
    char call_id[128];
    unsigned int cseq;
    unsigned int invite_cseq;
    uint32_t ssrc;
    uint16_t rtp_sequence;
    uint32_t rtp_timestamp;
    int registered;
    int invite_sent;
    int joined;
    int floor_request_pending;
    int floor_granted;
    int listening;
    char current_speaker[PTT_MAX_ID];
    int auto_request_sent;
    int auto_release_sent;
    uint64_t started_at_ms;
    uint64_t joined_at_ms;
    uint64_t next_rtp_keepalive_at_ms;
    uint64_t talk_started_at_ms;
    uint64_t next_rtp_at_ms;
    uint64_t transmitted_packets;
    uint64_t tx_packets_in_talk;
    uint64_t received_packets;
    uint64_t rejected_peer_packets;
    uint64_t rx_interval_started_ms;
    uint64_t rx_interval_packets;
    uint64_t rx_interval_payload_bytes;
    uint64_t rx_interval_latency_samples;
    uint64_t rx_interval_missing_timestamps;
    int64_t rx_interval_latency_sum_ms;
    int64_t rx_interval_latency_min_ms;
    int64_t rx_interval_latency_max_ms;
} client_state;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int signal_number)
{
    (void)signal_number;
    g_running = 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s --id ID --group GROUP --sip-port PORT --rtp-port PORT [options]\n"
            "Options:\n"
            "  --server HOST              default 123.57.79.87\n"
            "  --server-sip-port PORT     default 5060\n"
            "  --server-rtp-port PORT     default 40000\n"
            "  --run-seconds SEC          0 means interactive/unlimited\n"
            "  --auto-talk-delay-ms MS    request floor after joining\n"
            "  --auto-talk-ms MS          release floor after this duration\n"
            "  --opus-bitrate BPS         default 16000, simulated CBR payload\n",
            program);
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (end == text || *end != '\0' || value < 1 || value > 65535) {
        return 0;
    }
    *port = (uint16_t)value;
    return 1;
}

static int parse_arguments(int argc, char **argv, client_config *config)
{
    int i;

    memset(config, 0, sizeof(*config));
    config->server_host = "123.57.79.87";
    config->server_sip_port = 5060;
    config->server_rtp_port = 40000;
    config->opus_bitrate = 16000;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            config->server_host = argv[++i];
        } else if (strcmp(argv[i], "--server-sip-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->server_sip_port)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--server-rtp-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->server_rtp_port)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--sip-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->local_sip_port)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--rtp-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->local_rtp_port)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            strncpy(config->id, argv[++i], sizeof(config->id) - 1);
        } else if (strcmp(argv[i], "--group") == 0 && i + 1 < argc) {
            strncpy(config->group, argv[++i], sizeof(config->group) - 1);
        } else if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) {
            config->run_seconds = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--auto-talk-delay-ms") == 0 && i + 1 < argc) {
            config->auto_talk_delay_ms = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--auto-talk-ms") == 0 && i + 1 < argc) {
            config->auto_talk_ms = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--opus-bitrate") == 0 && i + 1 < argc) {
            config->opus_bitrate = (unsigned int)strtoul(argv[++i], NULL, 10);
            if (config->opus_bitrate < 6000 || config->opus_bitrate > 128000) {
                return 0;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            return 0;
        }
    }

    return config->id[0] != '\0' && config->group[0] != '\0' &&
           config->local_sip_port != 0 && config->local_rtp_port != 0;
}

static int resolve_ipv4(const char *host, uint16_t port, struct sockaddr_in *address)
{
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *item;
    char service[16];
    int result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof(service), "%u", port);
    result = getaddrinfo(host, service, &hints, &results);
    if (result != 0) {
        return 0;
    }

    result = 0;
    for (item = results; item != NULL; item = item->ai_next) {
        if (item->ai_addrlen == sizeof(*address)) {
            memcpy(address, item->ai_addr, sizeof(*address));
            result = 1;
            break;
        }
    }
    freeaddrinfo(results);
    return result;
}

static int bind_udp(uint16_t port)
{
    int fd;
    int reuse = 1;
    struct sockaddr_in address;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int same_endpoint(const struct sockaddr_in *left, const struct sockaddr_in *right)
{
    return left->sin_family == right->sin_family &&
           left->sin_port == right->sin_port &&
           left->sin_addr.s_addr == right->sin_addr.s_addr;
}

static size_t opus_payload_bytes(const client_state *state)
{
    const unsigned int bits_per_packet_divisor =
        8u * PTT_OPUS_PACKETS_PER_SECOND;

    return (state->config.opus_bitrate + bits_per_packet_divisor - 1u) /
           bits_per_packet_divisor;
}

static void print_opus_data_estimate(const client_state *state)
{
    size_t payload_bytes = opus_payload_bytes(state);
    uint64_t ipv4_bytes_per_second =
        (uint64_t)(payload_bytes + PTT_RTP_HEADER_WITH_SEND_TIME + 8u + 20u) *
        PTT_OPUS_PACKETS_PER_SECOND;
    double mebibytes_per_hour =
        (double)ipv4_bytes_per_second * 3600.0 / (1024.0 * 1024.0);

    printf("[%s] Opus transport: %u bit/s, %u packets/s, %u ms/packet, "
           "about %zu payload bytes/packet\n",
           state->config.id,
           state->config.opus_bitrate,
           PTT_OPUS_PACKETS_PER_SECOND,
           PTT_OPUS_PACKET_MS,
           payload_bytes);
    printf("[%s] estimated IPv4+UDP+RTP: %.2f kbit/s, %.2f MiB/hour "
           "for one stream direction\n",
           state->config.id,
           (double)ipv4_bytes_per_second * 8.0 / 1000.0,
           mebibytes_per_hour);
}

static void discover_local_ip(const struct sockaddr_in *server, char *output, size_t output_size)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local;
    socklen_t local_length = sizeof(local);

    strncpy(output, "127.0.0.1", output_size - 1);
    output[output_size - 1] = '\0';
    if (fd < 0) {
        return;
    }

    if (connect(fd, (const struct sockaddr *)server, sizeof(*server)) == 0 &&
        getsockname(fd, (struct sockaddr *)&local, &local_length) == 0) {
        inet_ntop(AF_INET, &local.sin_addr, output, output_size);
    }
    close(fd);
}

static unsigned int send_request(client_state *state,
                                 const char *method,
                                 const char *target,
                                 const char *content_type,
                                 const char *body,
                                 int increment_cseq)
{
    char message[PTT_MAX_SIP_PACKET];
    char extra[512];
    size_t body_length = body != NULL ? strlen(body) : 0;
    size_t used;
    unsigned int sequence;
    int length;

    if (increment_cseq) {
        ++state->cseq;
    }
    sequence = state->cseq;

    snprintf(extra,
             sizeof(extra),
             "X-Device-ID: %s\r\n"
             "X-Group-ID: %s\r\n"
             "X-RTP-Port: %u\r\n"
             "X-SSRC: %u\r\n",
             state->config.id,
             state->config.group,
             state->config.local_rtp_port,
             state->ssrc);

    length = snprintf(message,
                      sizeof(message),
                      "%s sip:%s@%s SIP/2.0\r\n"
                      "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-%u-%u\r\n"
                      "Max-Forwards: 70\r\n"
                      "From: <sip:%s@%s>;tag=%08x\r\n"
                      "To: <sip:%s@%s>\r\n"
                      "Call-ID: %s\r\n"
                      "CSeq: %u %s\r\n"
                      "Contact: <sip:%s@%s:%u>\r\n"
                      "%s"
                      "%s",
                      method,
                      target,
                      state->config.server_host,
                      state->local_ip,
                      state->config.local_sip_port,
                      sequence,
                      (unsigned int)(ptt_now_ms() & 0xffffu),
                      state->config.id,
                      state->config.server_host,
                      state->ssrc,
                      target,
                      state->config.server_host,
                      state->call_id,
                      sequence,
                      method,
                      state->config.id,
                      state->local_ip,
                      state->config.local_sip_port,
                      extra,
                      strcmp(method, "REGISTER") == 0 ? "Expires: 3600\r\n" : "");

    if (length <= 0 || (size_t)length >= sizeof(message)) {
        return 0;
    }
    used = (size_t)length;

    if (content_type != NULL) {
        length = snprintf(message + used,
                          sizeof(message) - used,
                          "Content-Type: %s\r\n",
                          content_type);
        if (length < 0 || (size_t)length >= sizeof(message) - used) {
            return 0;
        }
        used += (size_t)length;
    }

    length = snprintf(message + used,
                      sizeof(message) - used,
                      "Content-Length: %zu\r\n\r\n%s",
                      body_length,
                      body != NULL ? body : "");
    if (length < 0 || (size_t)length >= sizeof(message) - used) {
        return 0;
    }
    used += (size_t)length;

    if (sendto(state->sip_fd,
               message,
               used,
               0,
               (const struct sockaddr *)&state->server_sip_address,
               sizeof(state->server_sip_address)) < 0) {
        fprintf(stderr, "[%s] SIP send failed: %s\n", state->config.id, strerror(errno));
        return 0;
    }
    return sequence;
}

static void send_register(client_state *state)
{
    send_request(state, "REGISTER", state->config.id, NULL, NULL, 1);
    printf("[%s] -> REGISTER\n", state->config.id);
}

static void send_invite(client_state *state)
{
    char sdp[512];
    char target[PTT_MAX_GROUP + 16];

    snprintf(target, sizeof(target), "group-%s", state->config.group);
    snprintf(sdp,
             sizeof(sdp),
             "v=0\r\n"
             "o=%s 1 1 IN IP4 %s\r\n"
             "s=PTT client\r\n"
             "c=IN IP4 %s\r\n"
             "t=0 0\r\n"
             "m=audio %u RTP/AVP %u\r\n"
             "a=rtpmap:%u opus/48000/1\r\n"
             "a=fmtp:%u minptime=5;useinbandfec=1;maxaveragebitrate=%u\r\n"
             "a=ptime:25\r\n"
             "a=sendrecv\r\n",
             state->config.id,
             state->local_ip,
             state->local_ip,
             state->config.local_rtp_port,
             PTT_OPUS_PAYLOAD_TYPE,
             PTT_OPUS_PAYLOAD_TYPE,
             PTT_OPUS_PAYLOAD_TYPE,
             state->config.opus_bitrate);
    state->invite_cseq = send_request(state, "INVITE", target, "application/sdp", sdp, 1);
    state->invite_sent = 1;
    printf("[%s] -> INVITE group=%s\n", state->config.id, state->config.group);
}

static void send_ack(client_state *state)
{
    unsigned int saved = state->cseq;
    char target[PTT_MAX_GROUP + 16];

    snprintf(target, sizeof(target), "group-%s", state->config.group);
    state->cseq = state->invite_cseq;
    send_request(state, "ACK", target, NULL, NULL, 0);
    state->cseq = saved;
}

static void request_floor(client_state *state)
{
    char target[PTT_MAX_GROUP + 16];

    if (!state->joined || state->floor_request_pending || state->floor_granted) {
        printf("[%s] cannot request floor: joined=%d pending=%d granted=%d\n",
               state->config.id,
               state->joined,
               state->floor_request_pending,
               state->floor_granted);
        return;
    }
    snprintf(target, sizeof(target), "group-%s", state->config.group);
    send_request(state, "MESSAGE", target, "application/x-ptt", "PTT-REQUEST", 1);
    state->floor_request_pending = 1;
    printf("[%s] -> PTT-REQUEST\n", state->config.id);
}

static void release_floor(client_state *state)
{
    char target[PTT_MAX_GROUP + 16];

    if (!state->floor_granted) {
        return;
    }
    snprintf(target, sizeof(target), "group-%s", state->config.group);
    send_request(state, "MESSAGE", target, "application/x-ptt", "PTT-RELEASE", 1);
    state->floor_granted = 0;
    state->current_speaker[0] = '\0';
    printf("[%s] -> PTT-RELEASE\n", state->config.id);
}

static void send_bye(client_state *state)
{
    char target[PTT_MAX_GROUP + 16];

    if (!state->joined) {
        return;
    }
    snprintf(target, sizeof(target), "group-%s", state->config.group);
    send_request(state, "BYE", target, NULL, NULL, 1);
}

static void handle_sip_response(client_state *state, char *message)
{
    int status = 0;
    unsigned int sequence = 0;
    char method[32];
    char value[128];

    if (sscanf(message, "SIP/2.0 %d", &status) != 1 ||
        !ptt_parse_cseq(message, &sequence, method, sizeof(method))) {
        return;
    }

    if (strcasecmp(method, "REGISTER") == 0 && status == 200) {
        if (!state->registered) {
            state->registered = 1;
            printf("[%s] <- 200 REGISTER, registered\n", state->config.id);
            send_invite(state);
        }
    } else if (strcasecmp(method, "INVITE") == 0 && status == 200) {
        if (ptt_get_header(message, "X-Server-RTP-Port", value, sizeof(value))) {
            long port = strtol(value, NULL, 10);
            if (port > 0 && port <= 65535) {
                state->server_rtp_address.sin_port = htons((uint16_t)port);
            }
        }
        send_ack(state);
        if (!state->joined) {
            state->joined = 1;
            state->joined_at_ms = ptt_now_ms();
            state->next_rtp_keepalive_at_ms = state->joined_at_ms;
            printf("[%s] <- 200 INVITE, joined group=%s, state=IDLE; commands: talk, release, status, quit\n",
                   state->config.id,
                   state->config.group);
        }
    } else if (strcasecmp(method, "MESSAGE") == 0) {
        state->floor_request_pending = 0;
        if (ptt_get_header(message, "X-PTT-Result", value, sizeof(value))) {
            if (strcmp(value, "GRANTED") == 0) {
                state->floor_granted = 1;
                state->listening = 0;
                strncpy(state->current_speaker,
                        state->config.id,
                        sizeof(state->current_speaker) - 1);
                state->talk_started_at_ms = ptt_now_ms();
                state->next_rtp_at_ms = state->talk_started_at_ms;
                state->tx_packets_in_talk = 0;
                printf("[%s] <- floor GRANTED, state=TALKING, start sending "
                       "Opus-sized binary packets at 40 packets/s\n",
                       state->config.id);
            } else if (strcmp(value, "BUSY") == 0) {
                char owner[PTT_MAX_ID];
                if (!ptt_get_header(message, "X-PTT-Owner", owner, sizeof(owner))) {
                    strcpy(owner, "unknown");
                }
                printf("[%s] <- floor BUSY, owner=%s\n", state->config.id, owner);
            } else {
                printf("[%s] <- floor result=%s\n", state->config.id, value);
            }
        }
    } else if (status >= 300) {
        printf("[%s] <- SIP error %d for %s cseq=%u\n",
               state->config.id,
               status,
               method,
               sequence);
    }
}

static void send_response_to_server_request(client_state *state,
                                            const struct sockaddr_in *destination,
                                            const char *request,
                                            int status,
                                            const char *reason)
{
    char via[512];
    char from[512];
    char to[512];
    char call_id[256];
    char cseq[128];
    char response[PTT_MAX_SIP_PACKET];
    int length;

    if (!ptt_get_header(request, "Via", via, sizeof(via)) ||
        !ptt_get_header(request, "From", from, sizeof(from)) ||
        !ptt_get_header(request, "To", to, sizeof(to)) ||
        !ptt_get_header(request, "Call-ID", call_id, sizeof(call_id)) ||
        !ptt_get_header(request, "CSeq", cseq, sizeof(cseq))) {
        return;
    }

    length = snprintf(response,
                      sizeof(response),
                      "SIP/2.0 %d %s\r\n"
                      "Via: %s\r\n"
                      "From: %s\r\n"
                      "To: %s\r\n"
                      "Call-ID: %s\r\n"
                      "CSeq: %s\r\n"
                      "Content-Length: 0\r\n\r\n",
                      status,
                      reason,
                      via,
                      from,
                      to,
                      call_id,
                      cseq);
    if (length <= 0 || (size_t)length >= sizeof(response)) {
        return;
    }

    sendto(state->sip_fd,
           response,
           (size_t)length,
           0,
           (const struct sockaddr *)destination,
           sizeof(*destination));
}

static void handle_server_request(client_state *state,
                                  const struct sockaddr_in *source,
                                  char *message)
{
    char method[32];
    char group[PTT_MAX_GROUP];
    char speaker[PTT_MAX_ID];
    const char *body;

    if (sscanf(message, "%31s", method) != 1 || strcmp(method, "MESSAGE") != 0) {
        send_response_to_server_request(state, source, message, 405, "Method Not Allowed");
        return;
    }

    if (!ptt_get_header(message, "X-Group-ID", group, sizeof(group)) ||
        strcmp(group, state->config.group) != 0) {
        send_response_to_server_request(state, source, message, 400, "Wrong Group");
        return;
    }

    if (!ptt_get_header(message, "X-PTT-Speaker", speaker, sizeof(speaker))) {
        strncpy(speaker, "unknown", sizeof(speaker) - 1);
        speaker[sizeof(speaker) - 1] = '\0';
    }

    body = ptt_get_body(message);
    if (body != NULL && strncmp(body, "PTT-LISTEN", 10) == 0) {
        state->listening = 1;
        state->floor_granted = 0;
        strncpy(state->current_speaker, speaker, sizeof(state->current_speaker) - 1);
        state->current_speaker[sizeof(state->current_speaker) - 1] = '\0';
        printf("[%s] <- PTT-LISTEN speaker=%s, state=LISTENING\n",
               state->config.id,
               state->current_speaker);
        send_response_to_server_request(state, source, message, 200, "OK");
    } else if (body != NULL && strncmp(body, "PTT-IDLE", 8) == 0) {
        state->listening = 0;
        state->current_speaker[0] = '\0';
        printf("[%s] <- PTT-IDLE previous_speaker=%s, state=IDLE\n",
               state->config.id,
               speaker);
        send_response_to_server_request(state, source, message, 200, "OK");
    } else {
        send_response_to_server_request(state, source, message, 400, "Unknown Event");
    }
}

static void handle_sip_packet(client_state *state,
                              const struct sockaddr_in *source,
                              char *message)
{
    if (strncmp(message, "SIP/2.0", 7) == 0) {
        handle_sip_response(state, message);
    } else {
        handle_server_request(state, source, message);
    }
}

static void build_simulated_opus_payload(client_state *state,
                                         uint8_t *payload,
                                         size_t payload_length)
{
    size_t i;

    /*
     * Transport-only PC test data. It has the configured Opus-sized CBR length,
     * but is not codec output. Replace this function with the module/MCU Opus
     * encoder and pass its opaque packet bytes to ptt_build_rtp().
     */
    for (i = 0; i < payload_length; ++i) {
        payload[i] = (uint8_t)(state->rtp_sequence + i * 29u + (state->ssrc & 0xffu));
    }
}

static void send_rtp_payload(client_state *state)
{
    uint8_t payload[400];
    uint8_t packet[PTT_MAX_RTP_PACKET];
    size_t payload_length = opus_payload_bytes(state);
    size_t packet_length;
    uint16_t sequence = state->rtp_sequence;
    uint64_t send_time_ms = ptt_unix_time_ms();

    if (payload_length > sizeof(payload)) {
        return;
    }
    build_simulated_opus_payload(state, payload, payload_length);

    packet_length = ptt_build_rtp(packet,
                                  sizeof(packet),
                                  PTT_OPUS_PAYLOAD_TYPE,
                                  state->tx_packets_in_talk == 0,
                                  sequence,
                                  state->rtp_timestamp,
                                  state->ssrc,
                                  send_time_ms,
                                  payload,
                                  payload_length);
    ++state->rtp_sequence;
    state->rtp_timestamp += PTT_OPUS_TIMESTAMP_STEP;
    if (packet_length == 0) {
        return;
    }

    if (sendto(state->rtp_fd,
               packet,
               packet_length,
               0,
               (const struct sockaddr *)&state->server_rtp_address,
               sizeof(state->server_rtp_address)) >= 0) {
        ++state->transmitted_packets;
        ++state->tx_packets_in_talk;
    }
}

static void send_rtp_keepalive(client_state *state)
{
    uint8_t packet[PTT_MAX_RTP_PACKET];
    uint8_t empty_payload = 0;
    size_t packet_length;

    /*
     * This packet is sent from the client's RTP socket so the server can
     * learn the NAT-mapped RTP endpoint before the client receives audio.
     * It is not an Opus packet and is never forwarded to group listeners.
     */
    packet_length = ptt_build_rtp(packet,
                                  sizeof(packet),
                                  PTT_RTP_KEEPALIVE_PAYLOAD_TYPE,
                                  0,
                                  state->rtp_sequence,
                                  state->rtp_timestamp,
                                  state->ssrc,
                                  ptt_unix_time_ms(),
                                  &empty_payload,
                                  0);
    ++state->rtp_sequence;
    if (packet_length == 0) {
        return;
    }

    (void)sendto(state->rtp_fd,
                 packet,
                 packet_length,
                 0,
                 (const struct sockaddr *)&state->server_rtp_address,
                 sizeof(state->server_rtp_address));
}

static void print_receive_interval(client_state *state)
{
    if (state->rx_interval_packets == 0) {
        return;
    }

    if (state->rx_interval_latency_samples > 0) {
        printf("[%s] [rx/1s] via=server packets=%llu opus_payload=%llu bytes "
               "delay_ms avg=%.1f min=%lld max=%lld missing_timestamp=%llu\n",
               state->config.id,
               (unsigned long long)state->rx_interval_packets,
               (unsigned long long)state->rx_interval_payload_bytes,
               (double)state->rx_interval_latency_sum_ms /
                   (double)state->rx_interval_latency_samples,
               (long long)state->rx_interval_latency_min_ms,
               (long long)state->rx_interval_latency_max_ms,
               (unsigned long long)state->rx_interval_missing_timestamps);
    } else {
        printf("[%s] [rx/1s] via=server packets=%llu opus_payload=%llu bytes "
               "delay_ms unavailable missing_timestamp=%llu\n",
               state->config.id,
               (unsigned long long)state->rx_interval_packets,
               (unsigned long long)state->rx_interval_payload_bytes,
               (unsigned long long)state->rx_interval_missing_timestamps);
    }
}

static void reset_receive_interval(client_state *state, uint64_t now_ms)
{
    state->rx_interval_started_ms = now_ms;
    state->rx_interval_packets = 0;
    state->rx_interval_payload_bytes = 0;
    state->rx_interval_latency_samples = 0;
    state->rx_interval_missing_timestamps = 0;
    state->rx_interval_latency_sum_ms = 0;
    state->rx_interval_latency_min_ms = 0;
    state->rx_interval_latency_max_ms = 0;
}

static void receive_rtp(client_state *state)
{
    uint8_t packet[PTT_MAX_RTP_PACKET];
    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    ssize_t received;
    ptt_rtp_view view;
    uint64_t now_ms;
    uint64_t receive_time_ms;

    received = recvfrom(state->rtp_fd,
                        packet,
                        sizeof(packet),
                        0,
                        (struct sockaddr *)&source,
                        &source_length);
    if (received <= 0 || !ptt_parse_rtp(packet, (size_t)received, &view)) {
        return;
    }
    if (!same_endpoint(&source, &state->server_rtp_address)) {
        ++state->rejected_peer_packets;
        return;
    }
    if (view.payload_type != PTT_OPUS_PAYLOAD_TYPE) {
        return;
    }

    now_ms = ptt_now_ms();
    receive_time_ms = ptt_unix_time_ms();
    if (state->rx_interval_started_ms == 0) {
        reset_receive_interval(state, now_ms);
    } else if (now_ms - state->rx_interval_started_ms >= 1000u) {
        print_receive_interval(state);
        reset_receive_interval(state, now_ms);
    }
    ++state->received_packets;
    ++state->rx_interval_packets;
    state->rx_interval_payload_bytes += view.payload_length;
    if (view.has_send_time_ms) {
        int64_t latency_ms = (int64_t)receive_time_ms - (int64_t)view.send_time_ms;

        if (state->rx_interval_latency_samples == 0) {
            state->rx_interval_latency_min_ms = latency_ms;
            state->rx_interval_latency_max_ms = latency_ms;
        } else {
            if (latency_ms < state->rx_interval_latency_min_ms) {
                state->rx_interval_latency_min_ms = latency_ms;
            }
            if (latency_ms > state->rx_interval_latency_max_ms) {
                state->rx_interval_latency_max_ms = latency_ms;
            }
        }
        state->rx_interval_latency_sum_ms += latency_ms;
        ++state->rx_interval_latency_samples;
    } else {
        ++state->rx_interval_missing_timestamps;
    }
}

static void handle_command(client_state *state)
{
    char line[64];
    char command[32];

    if (fgets(line, sizeof(line), stdin) == NULL) {
        return;
    }

    if (sscanf(line, "%31s", command) != 1) {
        return;
    }

    if (strcasecmp(command, "p") == 0 || strcasecmp(command, "press") == 0 ||
        strcasecmp(command, "talk") == 0 || strcasecmp(command, "start") == 0) {
        request_floor(state);
    } else if (strcasecmp(command, "r") == 0 || strcasecmp(command, "release") == 0 ||
               strcasecmp(command, "stop") == 0) {
        release_floor(state);
    } else if (strcasecmp(command, "s") == 0 || strcasecmp(command, "status") == 0) {
        printf("[%s] status mode=%s speaker=%s registered=%d joined=%d floor=%d "
               "tx=%llu rx=%llu rejected_peer=%llu\n",
               state->config.id,
               state->floor_granted
                   ? "TALKING"
                   : (state->floor_request_pending
                          ? "REQUESTING"
                          : (state->listening ? "LISTENING" : "IDLE")),
               state->current_speaker[0] != '\0' ? state->current_speaker : "none",
               state->registered,
               state->joined,
               state->floor_granted,
               (unsigned long long)state->transmitted_packets,
               (unsigned long long)state->received_packets,
               (unsigned long long)state->rejected_peer_packets);
    } else if (strcasecmp(command, "q") == 0 || strcasecmp(command, "quit") == 0 ||
               strcasecmp(command, "exit") == 0) {
        g_running = 0;
    } else {
        printf("commands: talk, release, status, quit\n");
    }
}

static void run_timers(client_state *state)
{
    uint64_t now = ptt_now_ms();

    if (state->joined && !state->floor_granted &&
        now >= state->next_rtp_keepalive_at_ms) {
        send_rtp_keepalive(state);
        state->next_rtp_keepalive_at_ms = now + PTT_RTP_KEEPALIVE_INTERVAL_MS;
    }

    if (state->joined && state->config.auto_talk_ms > 0 && !state->auto_request_sent &&
        now - state->joined_at_ms >= state->config.auto_talk_delay_ms) {
        state->auto_request_sent = 1;
        request_floor(state);
    }

    if (state->floor_granted) {
        while (now >= state->next_rtp_at_ms) {
            send_rtp_payload(state);
            state->next_rtp_at_ms += PTT_OPUS_PACKET_MS;
            if (now > state->next_rtp_at_ms &&
                now - state->next_rtp_at_ms > 200) {
                state->next_rtp_at_ms = now + PTT_OPUS_PACKET_MS;
                break;
            }
        }

        if (state->config.auto_talk_ms > 0 && !state->auto_release_sent &&
            now - state->talk_started_at_ms >= state->config.auto_talk_ms) {
            state->auto_release_sent = 1;
            release_floor(state);
        }
    }

    if (state->config.run_seconds > 0 &&
        now - state->started_at_ms >= (uint64_t)state->config.run_seconds * 1000u) {
        g_running = 0;
    }
}

int main(int argc, char **argv)
{
    client_state state;

    memset(&state, 0, sizeof(state));
    if (!parse_arguments(argc, argv, &state.config)) {
        print_usage(argv[0]);
        return 2;
    }

    if (!resolve_ipv4(state.config.server_host,
                      state.config.server_sip_port,
                      &state.server_sip_address) ||
        !resolve_ipv4(state.config.server_host,
                      state.config.server_rtp_port,
                      &state.server_rtp_address)) {
        fprintf(stderr, "Cannot resolve server: %s\n", state.config.server_host);
        return 1;
    }

    state.sip_fd = bind_udp(state.config.local_sip_port);
    state.rtp_fd = bind_udp(state.config.local_rtp_port);
    if (state.sip_fd < 0 || state.rtp_fd < 0) {
        fprintf(stderr, "Cannot bind local UDP sockets: %s\n", strerror(errno));
        if (state.sip_fd >= 0) {
            close(state.sip_fd);
        }
        if (state.rtp_fd >= 0) {
            close(state.rtp_fd);
        }
        return 1;
    }

    discover_local_ip(&state.server_sip_address, state.local_ip, sizeof(state.local_ip));
    state.ssrc = ptt_hash_text(state.config.id);
    state.rtp_sequence = (uint16_t)(state.ssrc & 0xffffu);
    state.rtp_timestamp = state.ssrc;
    state.started_at_ms = ptt_now_ms();
    snprintf(state.call_id,
             sizeof(state.call_id),
             "%s-%ld-%llu@%s",
             state.config.id,
             (long)getpid(),
             (unsigned long long)state.started_at_ms,
             state.local_ip);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("[%s] SIP local=%s:%u RTP local=%s:%u server=%s:%u group=%s\n",
           state.config.id,
           state.local_ip,
           state.config.local_sip_port,
           state.local_ip,
           state.config.local_rtp_port,
           state.config.server_host,
           state.config.server_sip_port,
           state.config.group);
    printf("[%s] media route: this client sends only to server RTP %s:%u; "
           "peer-originated RTP is rejected\n",
           state.config.id,
           state.config.server_host,
           state.config.server_rtp_port);
    print_opus_data_estimate(&state);
    send_register(&state);

    while (g_running) {
        fd_set read_set;
        struct timeval timeout;
        int maximum_fd = state.sip_fd > state.rtp_fd ? state.sip_fd : state.rtp_fd;
        int selected;

        FD_ZERO(&read_set);
        FD_SET(state.sip_fd, &read_set);
        FD_SET(state.rtp_fd, &read_set);
        if (state.config.run_seconds == 0) {
            FD_SET(STDIN_FILENO, &read_set);
            if (STDIN_FILENO > maximum_fd) {
                maximum_fd = STDIN_FILENO;
            }
        }

        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;
        selected = select(maximum_fd + 1, &read_set, NULL, NULL, &timeout);
        if (selected < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "select failed: %s\n", strerror(errno));
            break;
        }

        if (FD_ISSET(state.sip_fd, &read_set)) {
            char message[PTT_MAX_SIP_PACKET];
            struct sockaddr_in source;
            socklen_t source_length = sizeof(source);
            ssize_t received = recvfrom(state.sip_fd,
                                        message,
                                        sizeof(message) - 1,
                                        0,
                                        (struct sockaddr *)&source,
                                        &source_length);
            if (received > 0) {
                message[received] = '\0';
                handle_sip_packet(&state, &source, message);
            }
        }

        if (FD_ISSET(state.rtp_fd, &read_set)) {
            receive_rtp(&state);
        }

        if (state.config.run_seconds == 0 && FD_ISSET(STDIN_FILENO, &read_set)) {
            handle_command(&state);
        }

        run_timers(&state);
    }

    release_floor(&state);
    send_bye(&state);
    close(state.sip_fd);
    close(state.rtp_fd);
    print_receive_interval(&state);
    printf("[%s] stopped, transmitted=%llu received=%llu rejected_peer=%llu\n",
           state.config.id,
           (unsigned long long)state.transmitted_packets,
           (unsigned long long)state.received_packets,
           (unsigned long long)state.rejected_peer_packets);
    return 0;
}

