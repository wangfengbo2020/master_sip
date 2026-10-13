#include "ptt_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

struct ptt_client {
    ptt_client_config config;
    int sip_fd;
    int rtp_fd;
    int started;
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
    int joined;
    int floor_request_pending;
    int floor_granted;
    int listening;
    int busy;
    char current_speaker[PTT_MAX_ID];
    ptt_client_mode mode;
    char detail[160];
    uint64_t next_rtp_keepalive_at_ms;
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
    int rx_sequence_valid;
    uint16_t rx_last_sequence;
    int rx_source_frame_valid;
    uint32_t rx_last_source_frame_id;
    int rx_capture_time_valid;
    uint64_t rx_last_capture_time_ms;
    uint32_t rx_last_timestamp;
    uint64_t rx_last_arrival_ms;
    uint64_t rx_interval_lost_packets;
    uint64_t rx_interval_duplicate_packets;
    uint64_t rx_interval_out_of_order_packets;
    uint64_t rx_interval_max_sequence_gap;
    uint64_t rx_interval_interarrival_samples;
    uint64_t rx_interval_interarrival_deviation_sum_ms;
    uint64_t rx_interval_max_interarrival_ms;
    uint64_t rx_interval_late_gaps;
    uint64_t rx_interval_drain_wakeups;
    uint64_t rx_interval_max_drain_packets;
    uint64_t rx_interval_kernel_drops;
    uint32_t rx_last_kernel_drop_count;
    int rx_kernel_drop_count_valid;
    size_t rx_interval_payload_min_bytes;
    size_t rx_interval_payload_max_bytes;
    uint64_t last_rx_packets_per_second;
    uint64_t last_rx_payload_bytes;
    double last_latency_average_ms;
    int64_t last_latency_min_ms;
    int64_t last_latency_max_ms;
};

static void emit_log(ptt_client *client, const char *format, ...)
{
    char line[512];
    va_list arguments;

    if (client == NULL || client->config.log_fn == NULL) {
        return;
    }
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    client->config.log_fn(client->config.log_user_data, line);
}

static void set_mode(ptt_client *client, ptt_client_mode mode, const char *format, ...)
{
    va_list arguments;

    client->mode = mode;
    if (format == NULL) {
        client->detail[0] = '\0';
        return;
    }
    va_start(arguments, format);
    vsnprintf(client->detail, sizeof(client->detail), format, arguments);
    va_end(arguments);
}

void ptt_client_config_init(ptt_client_config *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    strncpy(config->server_host, "123.57.79.87", sizeof(config->server_host) - 1);
    config->server_sip_port = 5060;
    config->server_rtp_port = 40000;
    config->opus_bitrate = 16000;
    config->simulate_opus = 0;
    config->self_loopback = 0;
}

const char *ptt_client_mode_name(ptt_client_mode mode)
{
    switch (mode) {
    case PTT_CLIENT_STOPPED: return "STOPPED";
    case PTT_CLIENT_CONNECTING: return "CONNECTING";
    case PTT_CLIENT_REGISTERING: return "REGISTERING";
    case PTT_CLIENT_JOINING: return "JOINING";
    case PTT_CLIENT_IDLE: return "IDLE";
    case PTT_CLIENT_REQUESTING: return "REQUESTING";
    case PTT_CLIENT_TALKING: return "TALKING";
    case PTT_CLIENT_LISTENING: return "LISTENING";
    case PTT_CLIENT_BUSY: return "BUSY";
    case PTT_CLIENT_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

ptt_client *ptt_client_create(const ptt_client_config *config)
{
    ptt_client *client;

    if (config == NULL || config->server_host[0] == '\0' ||
        config->device_id[0] == '\0' || config->group_id[0] == '\0' ||
        config->server_sip_port == 0 || config->server_rtp_port == 0 ||
        config->opus_bitrate < 6000 || config->opus_bitrate > 128000) {
        return NULL;
    }
    client = (ptt_client *)calloc(1, sizeof(*client));
    if (client == NULL) {
        return NULL;
    }
    client->config = *config;
    client->sip_fd = -1;
    client->rtp_fd = -1;
    client->mode = PTT_CLIENT_STOPPED;
    strncpy(client->detail, "尚未连接", sizeof(client->detail) - 1);
    return client;
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

static int bind_udp(uint16_t requested_port, uint16_t *actual_port)
{
    int fd;
    int reuse = 1;
    int socket_buffer = 1024 * 1024;
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    (void)setsockopt(fd,
                     SOL_SOCKET,
                     SO_RCVBUF,
                     &socket_buffer,
                     sizeof(socket_buffer));
    (void)setsockopt(fd,
                     SOL_SOCKET,
                     SO_SNDBUF,
                     &socket_buffer,
                     sizeof(socket_buffer));
#ifdef SO_RXQ_OVFL
    (void)setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &reuse, sizeof(reuse));
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(requested_port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &address_length) != 0) {
        close(fd);
        return -1;
    }
    *actual_port = ntohs(address.sin_port);
    return fd;
}

static int same_endpoint(const struct sockaddr_in *left, const struct sockaddr_in *right)
{
    return left->sin_family == right->sin_family &&
           left->sin_port == right->sin_port &&
           left->sin_addr.s_addr == right->sin_addr.s_addr;
}

static void discover_local_ip(const struct sockaddr_in *server,
                              char *output,
                              size_t output_size)
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
        (void)inet_ntop(AF_INET, &local.sin_addr, output, output_size);
    }
    close(fd);
}

static unsigned int send_request(ptt_client *client,
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
        ++client->cseq;
    }
    sequence = client->cseq;
    snprintf(extra,
             sizeof(extra),
             "X-Device-ID: %s\r\nX-Group-ID: %s\r\nX-RTP-Port: %u\r\n"
             "X-SSRC: %u\r\nX-RTP-Loopback: %d\r\n",
             client->config.device_id,
             client->config.group_id,
             client->config.local_rtp_port,
             client->ssrc,
             client->config.self_loopback ? 1 : 0);
    length = snprintf(message,
                      sizeof(message),
                      "%s sip:%s@%s SIP/2.0\r\n"
                      "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-%u-%u\r\n"
                      "Max-Forwards: 70\r\n"
                      "From: <sip:%s@%s>;tag=%08x\r\n"
                      "To: <sip:%s@%s>\r\n"
                      "Call-ID: %s\r\n"
                      "CSeq: %u %s\r\n"
                      "Contact: <sip:%s@%s:%u>\r\n%s%s",
                      method,
                      target,
                      client->config.server_host,
                      client->local_ip,
                      client->config.local_sip_port,
                      sequence,
                      (unsigned int)(ptt_now_ms() & 0xffffu),
                      client->config.device_id,
                      client->config.server_host,
                      client->ssrc,
                      target,
                      client->config.server_host,
                      client->call_id,
                      sequence,
                      method,
                      client->config.device_id,
                      client->local_ip,
                      client->config.local_sip_port,
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
    if (sendto(client->sip_fd,
               message,
               used,
               0,
               (const struct sockaddr *)&client->server_sip_address,
               sizeof(client->server_sip_address)) < 0) {
        set_mode(client, PTT_CLIENT_ERROR, "SIP发送失败: %s", strerror(errno));
        emit_log(client, "[%s] SIP send failed: %s", client->config.device_id, strerror(errno));
        return 0;
    }
    return sequence;
}

static void send_register(ptt_client *client)
{
    (void)send_request(client, "REGISTER", client->config.device_id, NULL, NULL, 1);
    set_mode(client, PTT_CLIENT_REGISTERING, "正在注册设备");
    emit_log(client, "[%s] -> REGISTER", client->config.device_id);
}

static void send_invite(ptt_client *client)
{
    char sdp[512];
    char target[PTT_MAX_GROUP + 16];

    snprintf(target, sizeof(target), "group-%s", client->config.group_id);
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
             "a=ptime:20\r\n"
             "a=sendrecv\r\n",
             client->config.device_id,
             client->local_ip,
             client->local_ip,
             client->config.local_rtp_port,
             PTT_OPUS_PAYLOAD_TYPE,
             PTT_OPUS_PAYLOAD_TYPE,
             PTT_OPUS_PAYLOAD_TYPE,
             client->config.opus_bitrate);
    client->invite_cseq = send_request(client,
                                       "INVITE",
                                       target,
                                       "application/sdp",
                                       sdp,
                                       1);
    set_mode(client, PTT_CLIENT_JOINING, "正在加入组 %s", client->config.group_id);
    emit_log(client, "[%s] -> INVITE group=%s",
             client->config.device_id,
             client->config.group_id);
}

static void send_ack(ptt_client *client)
{
    unsigned int saved = client->cseq;
    char target[PTT_MAX_GROUP + 16];

    snprintf(target, sizeof(target), "group-%s", client->config.group_id);
    client->cseq = client->invite_cseq;
    (void)send_request(client, "ACK", target, NULL, NULL, 0);
    client->cseq = saved;
}

static void send_bye(ptt_client *client)
{
    char target[PTT_MAX_GROUP + 16];

    if (!client->joined || client->sip_fd < 0) {
        return;
    }
    snprintf(target, sizeof(target), "group-%s", client->config.group_id);
    (void)send_request(client, "BYE", target, NULL, NULL, 1);
}

int ptt_client_start(ptt_client *client)
{
    if (client == NULL || client->started) {
        return -1;
    }
    set_mode(client, PTT_CLIENT_CONNECTING, "正在连接服务器");
    if (!resolve_ipv4(client->config.server_host,
                      client->config.server_sip_port,
                      &client->server_sip_address) ||
        !resolve_ipv4(client->config.server_host,
                      client->config.server_rtp_port,
                      &client->server_rtp_address)) {
        set_mode(client, PTT_CLIENT_ERROR, "无法解析服务器 %s", client->config.server_host);
        return -1;
    }
    client->sip_fd = bind_udp(client->config.local_sip_port,
                              &client->config.local_sip_port);
    client->rtp_fd = bind_udp(client->config.local_rtp_port,
                              &client->config.local_rtp_port);
    if (client->sip_fd < 0 || client->rtp_fd < 0) {
        set_mode(client, PTT_CLIENT_ERROR, "无法绑定本地UDP端口: %s", strerror(errno));
        if (client->sip_fd >= 0) close(client->sip_fd);
        if (client->rtp_fd >= 0) close(client->rtp_fd);
        client->sip_fd = -1;
        client->rtp_fd = -1;
        return -1;
    }
    discover_local_ip(&client->server_sip_address,
                      client->local_ip,
                      sizeof(client->local_ip));
    client->ssrc = ptt_hash_text(client->config.device_id);
    client->rtp_sequence = (uint16_t)(client->ssrc & 0xffffu);
    client->rtp_timestamp = client->ssrc;
    snprintf(client->call_id,
             sizeof(client->call_id),
             "%s-%ld-%llu@%s",
             client->config.device_id,
             (long)getpid(),
             (unsigned long long)ptt_now_ms(),
             client->local_ip);
    client->started = 1;
    emit_log(client,
             "[%s] SIP local=%s:%u RTP local=%s:%u server=%s:%u group=%s",
             client->config.device_id,
             client->local_ip,
             client->config.local_sip_port,
             client->local_ip,
             client->config.local_rtp_port,
             client->config.server_host,
             client->config.server_sip_port,
             client->config.group_id);
    send_register(client);
    return 0;
}

static void send_response_to_server_request(ptt_client *client,
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
                      "Via: %s\r\nFrom: %s\r\nTo: %s\r\n"
                      "Call-ID: %s\r\nCSeq: %s\r\nContent-Length: 0\r\n\r\n",
                      status,
                      reason,
                      via,
                      from,
                      to,
                      call_id,
                      cseq);
    if (length > 0 && (size_t)length < sizeof(response)) {
        (void)sendto(client->sip_fd,
                     response,
                     (size_t)length,
                     0,
                     (const struct sockaddr *)destination,
                     sizeof(*destination));
    }
}

static void handle_sip_response(ptt_client *client, char *message)
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
        if (!client->registered) {
            client->registered = 1;
            emit_log(client, "[%s] <- 200 REGISTER, registered", client->config.device_id);
            send_invite(client);
        }
    } else if (strcasecmp(method, "INVITE") == 0 && status == 200) {
        if (ptt_get_header(message, "X-Server-RTP-Port", value, sizeof(value))) {
            long port = strtol(value, NULL, 10);
            if (port > 0 && port <= 65535) {
                client->server_rtp_address.sin_port = htons((uint16_t)port);
            }
        }
        send_ack(client);
        if (!client->joined) {
            client->joined = 1;
            client->next_rtp_keepalive_at_ms = ptt_now_ms();
            set_mode(client, PTT_CLIENT_IDLE, "成功加入组 %s", client->config.group_id);
            emit_log(client,
                     "[%s] <- 200 INVITE, joined group=%s, state=IDLE",
                     client->config.device_id,
                     client->config.group_id);
        }
    } else if (strcasecmp(method, "MESSAGE") == 0) {
        client->floor_request_pending = 0;
        if (ptt_get_header(message, "X-PTT-Result", value, sizeof(value))) {
            if (strcmp(value, "GRANTED") == 0) {
                client->floor_granted = 1;
                client->listening = 0;
                client->busy = 0;
                strncpy(client->current_speaker,
                        client->config.device_id,
                        sizeof(client->current_speaker) - 1);
                client->next_rtp_at_ms = ptt_now_ms();
                client->tx_packets_in_talk = 0;
                set_mode(client, PTT_CLIENT_TALKING, "正在说话");
                emit_log(client,
                         "[%s] <- floor GRANTED, state=TALKING, start sending "
                         "20 ms Opus audio at 50 packets/s",
                         client->config.device_id);
            } else if (strcmp(value, "BUSY") == 0) {
                char owner[PTT_MAX_ID];
                if (!ptt_get_header(message, "X-PTT-Owner", owner, sizeof(owner))) {
                    strncpy(owner, "unknown", sizeof(owner) - 1);
                    owner[sizeof(owner) - 1] = '\0';
                }
                client->busy = 1;
                strncpy(client->current_speaker,
                        owner,
                        sizeof(client->current_speaker) - 1);
                client->current_speaker[sizeof(client->current_speaker) - 1] = '\0';
                set_mode(client, PTT_CLIENT_BUSY, "频道占用：%s 正在说话", owner);
                emit_log(client,
                         "[%s] <- floor BUSY, owner=%s",
                         client->config.device_id,
                         owner);
            } else if (strcmp(value, "RELEASED") == 0 ||
                       strcmp(value, "NOT-OWNER") == 0) {
                client->floor_granted = 0;
                client->busy = 0;
                client->current_speaker[0] = '\0';
                set_mode(client, PTT_CLIENT_IDLE, "成功加入组 %s", client->config.group_id);
                emit_log(client,
                         "[%s] <- floor result=%s",
                         client->config.device_id,
                         value);
            }
        }
    } else if (status >= 300) {
        set_mode(client, PTT_CLIENT_ERROR, "SIP错误 %d (%s)", status, method);
        emit_log(client,
                 "[%s] <- SIP error %d for %s cseq=%u",
                 client->config.device_id,
                 status,
                 method,
                 sequence);
    }
}

static void handle_server_request(ptt_client *client,
                                  const struct sockaddr_in *source,
                                  char *message)
{
    char method[32];
    char group[PTT_MAX_GROUP];
    char speaker[PTT_MAX_ID];
    const char *body;

    if (sscanf(message, "%31s", method) != 1 || strcmp(method, "MESSAGE") != 0) {
        send_response_to_server_request(client, source, message, 405, "Method Not Allowed");
        return;
    }
    if (!ptt_get_header(message, "X-Group-ID", group, sizeof(group)) ||
        strcmp(group, client->config.group_id) != 0) {
        send_response_to_server_request(client, source, message, 400, "Wrong Group");
        return;
    }
    if (!ptt_get_header(message, "X-PTT-Speaker", speaker, sizeof(speaker))) {
        strncpy(speaker, "unknown", sizeof(speaker) - 1);
        speaker[sizeof(speaker) - 1] = '\0';
    }
    body = ptt_get_body(message);
    if (body != NULL && strncmp(body, "PTT-LISTEN", 10) == 0) {
        client->listening = 1;
        client->floor_granted = 0;
        strncpy(client->current_speaker, speaker, sizeof(client->current_speaker) - 1);
        client->current_speaker[sizeof(client->current_speaker) - 1] = '\0';
        if (!client->busy) {
            set_mode(client, PTT_CLIENT_LISTENING, "正在收听：%s", speaker);
        }
        emit_log(client,
                 "[%s] <- PTT-LISTEN speaker=%s, state=LISTENING",
                 client->config.device_id,
                 speaker);
        send_response_to_server_request(client, source, message, 200, "OK");
    } else if (body != NULL && strncmp(body, "PTT-IDLE", 8) == 0) {
        client->listening = 0;
        client->busy = 0;
        client->current_speaker[0] = '\0';
        if (!client->floor_granted) {
            set_mode(client, PTT_CLIENT_IDLE, "成功加入组 %s", client->config.group_id);
        }
        emit_log(client,
                 "[%s] <- PTT-IDLE previous_speaker=%s, state=IDLE",
                 client->config.device_id,
                 speaker);
        send_response_to_server_request(client, source, message, 200, "OK");
    } else {
        send_response_to_server_request(client, source, message, 400, "Unknown Event");
    }
}

static void handle_sip_packet(ptt_client *client,
                              const struct sockaddr_in *source,
                              char *message)
{
    if (strncmp(message, "SIP/2.0", 7) == 0) {
        handle_sip_response(client, message);
    } else {
        handle_server_request(client, source, message);
    }
}

int ptt_client_request_talk(ptt_client *client)
{
    char target[PTT_MAX_GROUP + 16];

    if (client == NULL || !client->joined || client->floor_request_pending ||
        client->floor_granted) {
        return -1;
    }
    client->busy = 0;
    snprintf(target, sizeof(target), "group-%s", client->config.group_id);
    if (send_request(client,
                     "MESSAGE",
                     target,
                     "application/x-ptt",
                     "PTT-REQUEST",
                     1) == 0) {
        return -1;
    }
    client->floor_request_pending = 1;
    set_mode(client, PTT_CLIENT_REQUESTING, "正在申请讲话权");
    emit_log(client, "[%s] -> PTT-REQUEST", client->config.device_id);
    return 0;
}

int ptt_client_release_talk(ptt_client *client)
{
    char target[PTT_MAX_GROUP + 16];

    if (client == NULL || !client->floor_granted) {
        return -1;
    }
    snprintf(target, sizeof(target), "group-%s", client->config.group_id);
    if (send_request(client,
                     "MESSAGE",
                     target,
                     "application/x-ptt",
                     "PTT-RELEASE",
                     1) == 0) {
        return -1;
    }
    client->floor_granted = 0;
    client->current_speaker[0] = '\0';
    set_mode(client, PTT_CLIENT_IDLE, "成功加入组 %s", client->config.group_id);
    emit_log(client, "[%s] -> PTT-RELEASE", client->config.device_id);
    return 0;
}

static size_t opus_payload_bytes(const ptt_client *client)
{
    const unsigned int divisor = 8u * PTT_OPUS_PACKETS_PER_SECOND;
    return (client->config.opus_bitrate + divisor - 1u) / divisor;
}

static int send_rtp_payload(ptt_client *client,
                            const uint8_t *payload,
                            size_t payload_length,
                            uint32_t source_frame_id,
                            uint64_t capture_time_ms,
                            uint16_t *sent_sequence)
{
    uint8_t packet[PTT_MAX_RTP_PACKET];
    size_t packet_length;
    uint16_t sequence;
    uint32_t timestamp;
    uint64_t send_time_ms;
    int marker;
    int send_error = 0;
    ssize_t sent_bytes;

    if (!client->floor_granted || payload == NULL || payload_length == 0) {
        return -1;
    }
    sequence = client->rtp_sequence;
    timestamp = client->rtp_timestamp;
    send_time_ms = ptt_unix_time_ms();
    marker = client->tx_packets_in_talk == 0;
    packet_length = ptt_build_rtp_with_frame_metadata(
        packet,
        sizeof(packet),
        PTT_OPUS_PAYLOAD_TYPE,
        marker,
        sequence,
        timestamp,
        client->ssrc,
        send_time_ms,
        source_frame_id,
        capture_time_ms,
        payload,
        payload_length);
    ++client->rtp_sequence;
    client->rtp_timestamp += PTT_OPUS_TIMESTAMP_STEP;
    if (packet_length == 0) {
        return -1;
    }
    sent_bytes = sendto(client->rtp_fd,
                        packet,
                        packet_length,
                        0,
                        (const struct sockaddr *)&client->server_rtp_address,
                        sizeof(client->server_rtp_address));
    if (sent_bytes < 0) {
        send_error = errno;
        emit_log(client,
                 "[%s] [frame-tx-drop] capture_frame=%u seq=%u rtp_ts=%u "
                 "capture_ms=%llu send_ms=%llu capture_to_send_ms=%lld "
                 "payload=%zu marker=%d errno=%d error=%s",
                 client->config.device_id,
                 source_frame_id,
                 sequence,
                 timestamp,
                 (unsigned long long)capture_time_ms,
                 (unsigned long long)send_time_ms,
                 (long long)((int64_t)send_time_ms - (int64_t)capture_time_ms),
                 payload_length,
                 marker,
                 send_error,
                 strerror(send_error));
    }
    if (sent_bytes >= 0 && sent_sequence != NULL) {
        *sent_sequence = sequence;
    }
    if (client->config.send_packet_fn != NULL) {
        ptt_client_send_packet_info packet_info;

        packet_info.sequence = sequence;
        packet_info.rtp_timestamp = timestamp;
        packet_info.send_time_ms = send_time_ms;
        packet_info.source_frame_id = source_frame_id;
        packet_info.capture_time_ms = capture_time_ms;
        packet_info.payload_length = payload_length;
        packet_info.marker = marker;
        packet_info.succeeded = sent_bytes >= 0;
        packet_info.error_code = send_error;
        client->config.send_packet_fn(client->config.send_packet_user_data,
                                      &packet_info);
    }
    if (sent_bytes < 0) {
        return -1;
    }
    ++client->transmitted_packets;
    ++client->tx_packets_in_talk;
    emit_log(client,
             "[%s] [frame-tx] capture_frame=%u seq=%u rtp_ts=%u "
             "capture_ms=%llu send_ms=%llu capture_to_send_ms=%lld "
             "payload=%zu marker=%d continuous=1",
             client->config.device_id,
             source_frame_id,
             sequence,
             timestamp,
             (unsigned long long)capture_time_ms,
             (unsigned long long)send_time_ms,
             (long long)((int64_t)send_time_ms - (int64_t)capture_time_ms),
             payload_length,
             marker);
    return 0;
}

int ptt_client_send_opus(ptt_client *client,
                         const uint8_t *opus_packet,
                         size_t opus_packet_length)
{
    return ptt_client_send_opus_ex(client, opus_packet, opus_packet_length, NULL);
}

int ptt_client_send_opus_ex(ptt_client *client,
                            const uint8_t *opus_packet,
                            size_t opus_packet_length,
                            uint16_t *sent_sequence)
{
    if (client == NULL || client->config.simulate_opus) {
        return -1;
    }
    return ptt_client_send_opus_frame(client,
                                      opus_packet,
                                      opus_packet_length,
                                      (uint32_t)client->tx_packets_in_talk,
                                      ptt_unix_time_ms(),
                                      sent_sequence);
}

int ptt_client_send_opus_frame(ptt_client *client,
                               const uint8_t *opus_packet,
                               size_t opus_packet_length,
                               uint32_t source_frame_id,
                               uint64_t capture_time_ms,
                               uint16_t *sent_sequence)
{
    if (client == NULL || client->config.simulate_opus) {
        return -1;
    }
    return send_rtp_payload(client,
                            opus_packet,
                            opus_packet_length,
                            source_frame_id,
                            capture_time_ms,
                            sent_sequence);
}

static void send_simulated_opus(ptt_client *client)
{
    uint8_t payload[400];
    size_t payload_length = opus_payload_bytes(client);
    size_t i;

    if (payload_length > sizeof(payload)) {
        return;
    }
    for (i = 0; i < payload_length; ++i) {
        payload[i] = (uint8_t)(client->rtp_sequence + i * 29u + (client->ssrc & 0xffu));
    }
    (void)send_rtp_payload(client,
                           payload,
                           payload_length,
                           (uint32_t)client->tx_packets_in_talk,
                           ptt_unix_time_ms(),
                           NULL);
}

static void send_rtp_keepalive(ptt_client *client)
{
    uint8_t packet[PTT_MAX_RTP_PACKET];
    uint8_t empty_payload = 0;
    size_t packet_length;

    packet_length = ptt_build_rtp(packet,
                                  sizeof(packet),
                                  PTT_RTP_KEEPALIVE_PAYLOAD_TYPE,
                                  0,
                                  client->rtp_sequence,
                                  client->rtp_timestamp,
                                  client->ssrc,
                                  ptt_unix_time_ms(),
                                  &empty_payload,
                                  0);
    ++client->rtp_sequence;
    if (packet_length > 0) {
        (void)sendto(client->rtp_fd,
                     packet,
                     packet_length,
                     0,
                     (const struct sockaddr *)&client->server_rtp_address,
                     sizeof(client->server_rtp_address));
    }
}

static void finish_receive_interval(ptt_client *client)
{
    double average_payload_bytes;
    double average_interarrival_deviation_ms;

    if (client->rx_interval_packets == 0) {
        return;
    }
    client->last_rx_packets_per_second = client->rx_interval_packets;
    client->last_rx_payload_bytes = client->rx_interval_payload_bytes;
    average_payload_bytes =
        (double)client->rx_interval_payload_bytes /
        (double)client->rx_interval_packets;
    average_interarrival_deviation_ms =
        client->rx_interval_interarrival_samples > 0
            ? (double)client->rx_interval_interarrival_deviation_sum_ms /
                  (double)client->rx_interval_interarrival_samples
            : 0.0;
    if (client->rx_interval_latency_samples > 0) {
        client->last_latency_average_ms =
            (double)client->rx_interval_latency_sum_ms /
            (double)client->rx_interval_latency_samples;
        client->last_latency_min_ms = client->rx_interval_latency_min_ms;
        client->last_latency_max_ms = client->rx_interval_latency_max_ms;
        emit_log(client,
                 "[%s] [rx-net/1s] packets=%llu opus_bytes=%llu "
                 "payload_bytes avg=%.1f min=%zu max=%zu "
                 "seq_lost=%llu duplicate=%llu out_of_order=%llu max_seq_gap=%llu "
                 "arrival_ms max=%llu avg_deviation=%.1f late_gaps=%llu "
                 "drain_max=%llu kernel_drops=%llu "
                 "delay_ms avg=%.1f min=%lld max=%lld missing_timestamp=%llu",
                 client->config.device_id,
                 (unsigned long long)client->rx_interval_packets,
                 (unsigned long long)client->rx_interval_payload_bytes,
                 average_payload_bytes,
                 client->rx_interval_payload_min_bytes,
                 client->rx_interval_payload_max_bytes,
                 (unsigned long long)client->rx_interval_lost_packets,
                 (unsigned long long)client->rx_interval_duplicate_packets,
                 (unsigned long long)client->rx_interval_out_of_order_packets,
                 (unsigned long long)client->rx_interval_max_sequence_gap,
                 (unsigned long long)client->rx_interval_max_interarrival_ms,
                 average_interarrival_deviation_ms,
                 (unsigned long long)client->rx_interval_late_gaps,
                 (unsigned long long)client->rx_interval_max_drain_packets,
                 (unsigned long long)client->rx_interval_kernel_drops,
                 client->last_latency_average_ms,
                 (long long)client->rx_interval_latency_min_ms,
                 (long long)client->rx_interval_latency_max_ms,
                 (unsigned long long)client->rx_interval_missing_timestamps);
    } else {
        emit_log(client,
                 "[%s] [rx-net/1s] packets=%llu opus_bytes=%llu "
                 "payload_bytes avg=%.1f min=%zu max=%zu "
                 "seq_lost=%llu duplicate=%llu out_of_order=%llu max_seq_gap=%llu "
                 "arrival_ms max=%llu avg_deviation=%.1f late_gaps=%llu "
                 "drain_max=%llu kernel_drops=%llu "
                 "delay_ms unavailable missing_timestamp=%llu",
                 client->config.device_id,
                 (unsigned long long)client->rx_interval_packets,
                 (unsigned long long)client->rx_interval_payload_bytes,
                 average_payload_bytes,
                 client->rx_interval_payload_min_bytes,
                 client->rx_interval_payload_max_bytes,
                 (unsigned long long)client->rx_interval_lost_packets,
                 (unsigned long long)client->rx_interval_duplicate_packets,
                 (unsigned long long)client->rx_interval_out_of_order_packets,
                 (unsigned long long)client->rx_interval_max_sequence_gap,
                 (unsigned long long)client->rx_interval_max_interarrival_ms,
                 average_interarrival_deviation_ms,
                 (unsigned long long)client->rx_interval_late_gaps,
                 (unsigned long long)client->rx_interval_max_drain_packets,
                 (unsigned long long)client->rx_interval_kernel_drops,
                 (unsigned long long)client->rx_interval_missing_timestamps);
    }
}

static void reset_receive_interval(ptt_client *client, uint64_t now_ms)
{
    client->rx_interval_started_ms = now_ms;
    client->rx_interval_packets = 0;
    client->rx_interval_payload_bytes = 0;
    client->rx_interval_latency_samples = 0;
    client->rx_interval_missing_timestamps = 0;
    client->rx_interval_latency_sum_ms = 0;
    client->rx_interval_latency_min_ms = 0;
    client->rx_interval_latency_max_ms = 0;
    client->rx_interval_lost_packets = 0;
    client->rx_interval_duplicate_packets = 0;
    client->rx_interval_out_of_order_packets = 0;
    client->rx_interval_max_sequence_gap = 0;
    client->rx_interval_interarrival_samples = 0;
    client->rx_interval_interarrival_deviation_sum_ms = 0;
    client->rx_interval_max_interarrival_ms = 0;
    client->rx_interval_late_gaps = 0;
    client->rx_interval_drain_wakeups = 0;
    client->rx_interval_max_drain_packets = 0;
    client->rx_interval_kernel_drops = 0;
    client->rx_interval_payload_min_bytes = 0;
    client->rx_interval_payload_max_bytes = 0;
}

static void record_receive_sequence(ptt_client *client,
                                    const ptt_rtp_view *view,
                                    uint64_t now_ms)
{
    uint16_t sequence_delta;

    if (!client->rx_sequence_valid || view->marker) {
        client->rx_sequence_valid = 1;
        client->rx_last_sequence = view->sequence;
        client->rx_last_timestamp = view->timestamp;
        client->rx_last_arrival_ms = now_ms;
        return;
    }

    sequence_delta = (uint16_t)(view->sequence - client->rx_last_sequence);
    if (sequence_delta == 0) {
        ++client->rx_interval_duplicate_packets;
        return;
    }
    if (sequence_delta >= 0x8000u) {
        ++client->rx_interval_out_of_order_packets;
        return;
    }

    {
        uint64_t arrival_delta_ms = now_ms - client->rx_last_arrival_ms;
        uint32_t timestamp_delta = view->timestamp - client->rx_last_timestamp;
        uint64_t expected_delta_ms =
            ((uint64_t)timestamp_delta * 1000u) / PTT_OPUS_SAMPLE_RATE;
        uint64_t deviation_ms = arrival_delta_ms >= expected_delta_ms
                                    ? arrival_delta_ms - expected_delta_ms
                                    : expected_delta_ms - arrival_delta_ms;

        if (sequence_delta > 1u) {
            uint64_t lost = (uint64_t)sequence_delta - 1u;
            client->rx_interval_lost_packets += lost;
            if (lost > client->rx_interval_max_sequence_gap) {
                client->rx_interval_max_sequence_gap = lost;
            }
        }
        if (arrival_delta_ms > client->rx_interval_max_interarrival_ms) {
            client->rx_interval_max_interarrival_ms = arrival_delta_ms;
        }
        client->rx_interval_interarrival_deviation_sum_ms += deviation_ms;
        ++client->rx_interval_interarrival_samples;
        if (arrival_delta_ms > expected_delta_ms + 10u) {
            ++client->rx_interval_late_gaps;
        }
    }

    client->rx_last_sequence = view->sequence;
    client->rx_last_timestamp = view->timestamp;
    client->rx_last_arrival_ms = now_ms;
}

static int receive_rtp(ptt_client *client)
{
    uint8_t packet[PTT_MAX_RTP_PACKET];
    struct sockaddr_in source;
    struct iovec io_vector;
    struct msghdr message;
#ifdef SO_RXQ_OVFL
    char control[CMSG_SPACE(sizeof(uint32_t))];
    struct cmsghdr *control_message;
#endif
    ssize_t received;
    ptt_rtp_view view;
    uint64_t now_ms;
    uint64_t arrival_unix_ms;
    uint16_t previous_sequence;
    uint16_t sequence_delta;
    int previous_valid;
    uint32_t previous_source_frame_id;
    uint32_t source_frame_delta;
    int source_previous_valid;
    int source_continuous;
    uint64_t arrival_gap_ms;
    uint64_t capture_gap_ms;
    int rtp_continuous;
    int continuous;
    const char *sequence_status;

    memset(&message, 0, sizeof(message));
    io_vector.iov_base = packet;
    io_vector.iov_len = sizeof(packet);
    message.msg_name = &source;
    message.msg_namelen = sizeof(source);
    message.msg_iov = &io_vector;
    message.msg_iovlen = 1;
#ifdef SO_RXQ_OVFL
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
#endif
    received = recvmsg(client->rtp_fd, &message, MSG_DONTWAIT);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
#ifdef SO_RXQ_OVFL
    for (control_message = CMSG_FIRSTHDR(&message);
         control_message != NULL;
         control_message = CMSG_NXTHDR(&message, control_message)) {
        if (control_message->cmsg_level == SOL_SOCKET &&
            control_message->cmsg_type == SO_RXQ_OVFL &&
            control_message->cmsg_len >= CMSG_LEN(sizeof(uint32_t))) {
            uint32_t cumulative_drops;
            memcpy(&cumulative_drops,
                   CMSG_DATA(control_message),
                   sizeof(cumulative_drops));
            if (client->rx_kernel_drop_count_valid) {
                client->rx_interval_kernel_drops +=
                    (uint32_t)(cumulative_drops - client->rx_last_kernel_drop_count);
            } else {
                client->rx_interval_kernel_drops += cumulative_drops;
            }
            client->rx_last_kernel_drop_count = cumulative_drops;
            client->rx_kernel_drop_count_valid = 1;
        }
    }
#endif
    if (received <= 0 || !ptt_parse_rtp(packet, (size_t)received, &view)) {
        return 1;
    }
    if (!same_endpoint(&source, &client->server_rtp_address)) {
        ++client->rejected_peer_packets;
        return 1;
    }
    if (view.payload_type != PTT_OPUS_PAYLOAD_TYPE) {
        return 1;
    }
    now_ms = ptt_now_ms();
    arrival_unix_ms = ptt_unix_time_ms();
    if (client->rx_interval_started_ms == 0) {
        reset_receive_interval(client, now_ms);
    } else if (now_ms - client->rx_interval_started_ms >= 1000u) {
        finish_receive_interval(client);
        reset_receive_interval(client, now_ms);
    }
    ++client->received_packets;
    ++client->rx_interval_packets;
    client->rx_interval_payload_bytes += view.payload_length;
    if (client->rx_interval_payload_min_bytes == 0 ||
        view.payload_length < client->rx_interval_payload_min_bytes) {
        client->rx_interval_payload_min_bytes = view.payload_length;
    }
    if (view.payload_length > client->rx_interval_payload_max_bytes) {
        client->rx_interval_payload_max_bytes = view.payload_length;
    }
    previous_valid = client->rx_sequence_valid && !view.marker;
    previous_sequence = client->rx_last_sequence;
    arrival_gap_ms = previous_valid && now_ms >= client->rx_last_arrival_ms
                         ? now_ms - client->rx_last_arrival_ms
                         : 0u;
    sequence_delta = previous_valid
                         ? (uint16_t)(view.sequence - previous_sequence)
                         : 0u;
    if (!previous_valid) {
        rtp_continuous = 1;
        sequence_status = view.marker ? "start" : "first";
    } else if (sequence_delta == 1u) {
        rtp_continuous = 1;
        sequence_status = "continuous";
    } else if (sequence_delta == 0u) {
        rtp_continuous = 0;
        sequence_status = "duplicate";
    } else if (sequence_delta >= 0x8000u) {
        rtp_continuous = 0;
        sequence_status = "out_of_order";
    } else {
        rtp_continuous = 0;
        sequence_status = "gap";
    }
    previous_source_frame_id = client->rx_last_source_frame_id;
    source_previous_valid = view.has_source_frame_id &&
                            client->rx_source_frame_valid && !view.marker;
    source_frame_delta = source_previous_valid
                             ? view.source_frame_id - previous_source_frame_id
                             : 0u;
    source_continuous = !source_previous_valid || source_frame_delta == 1u;
    capture_gap_ms = view.has_capture_time_ms &&
                             client->rx_capture_time_valid && !view.marker &&
                             view.capture_time_ms >= client->rx_last_capture_time_ms
                         ? view.capture_time_ms - client->rx_last_capture_time_ms
                         : 0u;
    continuous = rtp_continuous && source_continuous;
    emit_log(client,
             "[%s] [frame-rx] capture_frame=%u has_capture_frame=%d seq=%u "
             "arrival_ms=%llu capture_ms=%llu send_ms=%llu age_ms=%lld "
             "capture_to_send_ms=%lld arrival_gap_ms=%llu capture_gap_ms=%llu "
             "previous=%u previous_valid=%d delta=%u source_previous=%u "
             "source_previous_valid=%d source_delta=%u rtp_continuous=%d "
             "source_continuous=%d continuous=%d marker=%u status=%s payload=%zu",
             client->config.device_id,
             view.source_frame_id,
             view.has_source_frame_id,
             view.sequence,
             (unsigned long long)arrival_unix_ms,
             (unsigned long long)(view.has_capture_time_ms
                                      ? view.capture_time_ms
                                      : 0u),
             (unsigned long long)(view.has_send_time_ms ? view.send_time_ms : 0u),
             (long long)(view.has_send_time_ms
                             ? (int64_t)arrival_unix_ms -
                                   (int64_t)view.send_time_ms
                             : -1),
             (long long)(view.has_send_time_ms && view.has_capture_time_ms
                             ? (int64_t)view.send_time_ms -
                                   (int64_t)view.capture_time_ms
                             : -1),
             (unsigned long long)arrival_gap_ms,
             (unsigned long long)capture_gap_ms,
             previous_sequence,
             previous_valid,
             sequence_delta,
             previous_source_frame_id,
             source_previous_valid,
             source_frame_delta,
             rtp_continuous,
             source_continuous,
             continuous,
             view.marker,
             sequence_status,
             view.payload_length);
    if (client->config.receive_packet_fn != NULL) {
        ptt_client_receive_packet_info packet_info;

        memset(&packet_info, 0, sizeof(packet_info));
        packet_info.sequence = view.sequence;
        packet_info.previous_sequence = previous_sequence;
        packet_info.sequence_delta = sequence_delta;
        packet_info.arrival_time_ms = arrival_unix_ms;
        packet_info.payload_length = view.payload_length;
        packet_info.previous_valid = previous_valid;
        packet_info.continuous = continuous;
        packet_info.marker = view.marker != 0;
        packet_info.has_source_frame_id = view.has_source_frame_id;
        packet_info.source_frame_id = view.source_frame_id;
        packet_info.previous_source_frame_id = previous_source_frame_id;
        packet_info.source_frame_delta = source_frame_delta;
        packet_info.source_previous_valid = source_previous_valid;
        packet_info.source_continuous = source_continuous;
        packet_info.has_capture_time_ms = view.has_capture_time_ms;
        packet_info.capture_time_ms = view.capture_time_ms;
        packet_info.has_send_time_ms = view.has_send_time_ms;
        packet_info.send_time_ms = view.send_time_ms;
        packet_info.transport_delay_ms = view.has_send_time_ms
                                             ? (int64_t)arrival_unix_ms -
                                                   (int64_t)view.send_time_ms
                                             : -1;
        packet_info.arrival_gap_ms = arrival_gap_ms;
        packet_info.capture_gap_ms = capture_gap_ms;
        packet_info.status = sequence_status;
        client->config.receive_packet_fn(
            client->config.receive_packet_user_data,
            &packet_info);
    }
    if (view.has_source_frame_id &&
        (!source_previous_valid ||
         (source_frame_delta > 0u && source_frame_delta < 0x80000000u))) {
        client->rx_source_frame_valid = 1;
        client->rx_last_source_frame_id = view.source_frame_id;
    }
    if (view.has_capture_time_ms) {
        client->rx_capture_time_valid = 1;
        client->rx_last_capture_time_ms = view.capture_time_ms;
    }
    record_receive_sequence(client, &view, now_ms);
    if (view.has_send_time_ms) {
        int64_t latency_ms =
            (int64_t)arrival_unix_ms - (int64_t)view.send_time_ms;
        if (client->rx_interval_latency_samples == 0) {
            client->rx_interval_latency_min_ms = latency_ms;
            client->rx_interval_latency_max_ms = latency_ms;
        } else {
            if (latency_ms < client->rx_interval_latency_min_ms) {
                client->rx_interval_latency_min_ms = latency_ms;
            }
            if (latency_ms > client->rx_interval_latency_max_ms) {
                client->rx_interval_latency_max_ms = latency_ms;
            }
        }
        client->rx_interval_latency_sum_ms += latency_ms;
        ++client->rx_interval_latency_samples;
    } else {
        ++client->rx_interval_missing_timestamps;
    }
    if (client->config.opus_receive_fn != NULL) {
        client->config.opus_receive_fn(client->config.opus_receive_user_data,
                                       view.payload,
                                       view.payload_length,
                                       view.sequence,
                                       view.marker != 0);
    }
    return 1;
}

static void run_timers(ptt_client *client)
{
    uint64_t now = ptt_now_ms();

    if (client->joined && !client->floor_granted &&
        now >= client->next_rtp_keepalive_at_ms) {
        send_rtp_keepalive(client);
        client->next_rtp_keepalive_at_ms = now + PTT_RTP_KEEPALIVE_INTERVAL_MS;
    }
    if (client->floor_granted && client->config.simulate_opus) {
        while (now >= client->next_rtp_at_ms) {
            send_simulated_opus(client);
            client->next_rtp_at_ms += PTT_OPUS_PACKET_MS;
            if (now > client->next_rtp_at_ms && now - client->next_rtp_at_ms > 200u) {
                client->next_rtp_at_ms = now + PTT_OPUS_PACKET_MS;
                break;
            }
        }
    }
    if (client->rx_interval_started_ms > 0 &&
        now - client->rx_interval_started_ms >= 1000u) {
        finish_receive_interval(client);
        reset_receive_interval(client, now);
    }
}

int ptt_client_poll(ptt_client *client, unsigned int timeout_ms)
{
    fd_set read_set;
    struct timeval timeout;
    int maximum_fd;
    int selected;

    if (client == NULL || !client->started) {
        return -1;
    }
    if (timeout_ms > 1000u) {
        timeout_ms = 1000u;
    }
    FD_ZERO(&read_set);
    FD_SET(client->sip_fd, &read_set);
    FD_SET(client->rtp_fd, &read_set);
    maximum_fd = client->sip_fd > client->rtp_fd ? client->sip_fd : client->rtp_fd;
    timeout.tv_sec = (long)(timeout_ms / 1000u);
    timeout.tv_usec = (long)(timeout_ms % 1000u) * 1000L;
    selected = select(maximum_fd + 1, &read_set, NULL, NULL, &timeout);
    if (selected < 0) {
        if (errno == EINTR) {
            return 0;
        }
        set_mode(client, PTT_CLIENT_ERROR, "网络轮询失败: %s", strerror(errno));
        return -1;
    }
    if (FD_ISSET(client->sip_fd, &read_set)) {
        char message[PTT_MAX_SIP_PACKET];
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        ssize_t received = recvfrom(client->sip_fd,
                                    message,
                                    sizeof(message) - 1,
                                    0,
                                    (struct sockaddr *)&source,
                                    &source_length);
        if (received > 0) {
            message[received] = '\0';
            handle_sip_packet(client, &source, message);
        }
    }
    if (FD_ISSET(client->rtp_fd, &read_set)) {
        uint64_t drained_packets = 0;
        int receive_result;

        do {
            receive_result = receive_rtp(client);
            if (receive_result > 0) {
                ++drained_packets;
            }
        } while (receive_result > 0);
        if (drained_packets > 0) {
            ++client->rx_interval_drain_wakeups;
            if (drained_packets > client->rx_interval_max_drain_packets) {
                client->rx_interval_max_drain_packets = drained_packets;
            }
        }
    }
    run_timers(client);
    return 0;
}

void ptt_client_get_snapshot(const ptt_client *client, ptt_client_snapshot *snapshot)
{
    if (client == NULL || snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->mode = client->mode;
    snapshot->registered = client->registered;
    snapshot->joined = client->joined;
    snapshot->floor_granted = client->floor_granted;
    strncpy(snapshot->speaker, client->current_speaker, sizeof(snapshot->speaker) - 1);
    strncpy(snapshot->detail, client->detail, sizeof(snapshot->detail) - 1);
    snapshot->local_sip_port = client->config.local_sip_port;
    snapshot->local_rtp_port = client->config.local_rtp_port;
    snapshot->transmitted_packets = client->transmitted_packets;
    snapshot->received_packets = client->received_packets;
    snapshot->rejected_peer_packets = client->rejected_peer_packets;
    snapshot->last_rx_packets_per_second = client->last_rx_packets_per_second;
    snapshot->last_rx_payload_bytes = client->last_rx_payload_bytes;
    snapshot->last_latency_average_ms = client->last_latency_average_ms;
    snapshot->last_latency_min_ms = client->last_latency_min_ms;
    snapshot->last_latency_max_ms = client->last_latency_max_ms;
}

void ptt_client_stop(ptt_client *client)
{
    if (client == NULL || !client->started) {
        return;
    }
    if (client->floor_granted) {
        (void)ptt_client_release_talk(client);
    }
    send_bye(client);
    if (client->sip_fd >= 0) close(client->sip_fd);
    if (client->rtp_fd >= 0) close(client->rtp_fd);
    client->sip_fd = -1;
    client->rtp_fd = -1;
    client->started = 0;
    client->registered = 0;
    client->joined = 0;
    client->floor_granted = 0;
    finish_receive_interval(client);
    set_mode(client, PTT_CLIENT_STOPPED, "连接已停止");
    emit_log(client,
             "[%s] stopped, transmitted=%llu received=%llu rejected_peer=%llu",
             client->config.device_id,
             (unsigned long long)client->transmitted_packets,
             (unsigned long long)client->received_packets,
             (unsigned long long)client->rejected_peer_packets);
}

void ptt_client_destroy(ptt_client *client)
{
    if (client == NULL) {
        return;
    }
    ptt_client_stop(client);
    free(client);
}
