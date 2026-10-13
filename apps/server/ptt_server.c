#include "ptt_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_MAX_CLIENTS 64
#define SERVER_MAX_FLOORS 16
#define SERVER_PACKET_HISTORY_CAPACITY (PTT_OPUS_PACKETS_PER_SECOND * 180u)

typedef struct {
    int used;
    int registered;
    int joined;
    char id[PTT_MAX_ID];
    char group[PTT_MAX_GROUP];
    struct sockaddr_in sip_address;
    struct sockaddr_in rtp_address;
    uint32_t ssrc;
    int self_loopback;
    uint64_t expires_at_ms;
    uint64_t forwarded_packets;
    int rx_sequence_valid;
    uint16_t rx_last_sequence;
    int rx_source_frame_valid;
    uint32_t rx_last_source_frame_id;
    int rx_capture_time_valid;
    uint64_t rx_last_capture_time_ms;
} server_client;

typedef struct {
    int used;
    char group[PTT_MAX_GROUP];
    int owner_index;
    uint64_t granted_at_ms;
    uint64_t last_rtp_ms;
} group_floor;

typedef struct {
    const char *bind_ip;
    uint16_t sip_port;
    uint16_t rtp_port;
    unsigned int floor_timeout_seconds;
    unsigned int run_seconds;
} server_config;

typedef struct {
    uint64_t receive_ms;
    uint64_t phone_send_ms;
    uint32_t source_frame_id;
    uint16_t sequence;
    uint16_t payload_length;
    uint16_t recipients;
    uint16_t send_errors;
    uint8_t continuous;
    uint8_t marker;
    char device_id[PTT_MAX_ID];
    char group[PTT_MAX_GROUP];
    char status[16];
} server_packet_history_entry;

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_dump_packet_history = 0;
static server_client g_clients[SERVER_MAX_CLIENTS];
static group_floor g_floors[SERVER_MAX_FLOORS];
static int g_sip_fd = -1;
static unsigned int g_server_cseq = 1;
static uint64_t g_rtp_stats_started_ms;
static uint64_t g_rtp_stats_received_packets;
static uint64_t g_rtp_stats_forwarded_copies;
static uint64_t g_rtp_stats_send_errors;
static uint64_t g_rtp_stats_max_drain_packets;
static server_packet_history_entry
    g_packet_history[SERVER_PACKET_HISTORY_CAPACITY];
static size_t g_packet_history_write;
static size_t g_packet_history_count;

static void on_signal(int signal_number)
{
    if (signal_number == SIGUSR1) {
        g_dump_packet_history = 1;
    } else {
        g_running = 0;
    }
}

static void append_packet_history(const server_client *client,
                                  const ptt_rtp_view *view,
                                  uint64_t receive_ms,
                                  unsigned int recipients,
                                  unsigned int send_errors,
                                  int continuous,
                                  const char *status)
{
    server_packet_history_entry *entry =
        &g_packet_history[g_packet_history_write];

    memset(entry, 0, sizeof(*entry));
    entry->receive_ms = receive_ms;
    entry->phone_send_ms = view->has_send_time_ms ? view->send_time_ms : 0u;
    entry->source_frame_id = view->source_frame_id;
    entry->sequence = view->sequence;
    entry->payload_length =
        view->payload_length <= UINT16_MAX
            ? (uint16_t)view->payload_length
            : UINT16_MAX;
    entry->recipients = recipients <= UINT16_MAX
                            ? (uint16_t)recipients
                            : UINT16_MAX;
    entry->send_errors = send_errors <= UINT16_MAX
                             ? (uint16_t)send_errors
                             : UINT16_MAX;
    entry->continuous = continuous ? 1u : 0u;
    entry->marker = view->marker ? 1u : 0u;
    snprintf(entry->device_id, sizeof(entry->device_id), "%s", client->id);
    snprintf(entry->group, sizeof(entry->group), "%s", client->group);
    snprintf(entry->status,
             sizeof(entry->status),
             "%s",
             status != NULL ? status : "unknown");
    g_packet_history_write =
        (g_packet_history_write + 1u) % SERVER_PACKET_HISTORY_CAPACITY;
    if (g_packet_history_count < SERVER_PACKET_HISTORY_CAPACITY) {
        ++g_packet_history_count;
    }
}

static void dump_packet_history(void)
{
    size_t i;
    size_t first =
        (g_packet_history_write + SERVER_PACKET_HISTORY_CAPACITY -
         g_packet_history_count) %
        SERVER_PACKET_HISTORY_CAPACITY;

    printf("[history-dump] version=%s packets=%zu capacity=%u "
           "window_seconds=180\n",
           PTT_PRODUCT_VERSION,
           g_packet_history_count,
           (unsigned int)SERVER_PACKET_HISTORY_CAPACITY);
    for (i = 0; i < g_packet_history_count; ++i) {
        const server_packet_history_entry *entry =
            &g_packet_history[(first + i) % SERVER_PACKET_HISTORY_CAPACITY];
        printf("[history-frame] receive_ms=%llu phone_send_ms=%llu "
               "group=%s from=%s capture_frame=%u seq=%u payload=%u "
               "recipients=%u send_errors=%u continuous=%u marker=%u "
               "status=%s\n",
               (unsigned long long)entry->receive_ms,
               (unsigned long long)entry->phone_send_ms,
               entry->group,
               entry->device_id,
               entry->source_frame_id,
               entry->sequence,
               entry->payload_length,
               entry->recipients,
               entry->send_errors,
               entry->continuous,
               entry->marker,
               entry->status);
    }
    printf("[history-dump-end] packets=%zu\n", g_packet_history_count);
    fflush(stdout);
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--bind IP] [--sip-port PORT] [--rtp-port PORT]\n"
            "          [--floor-timeout SEC] [--run-seconds SEC]\n",
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

static int parse_arguments(int argc, char **argv, server_config *config)
{
    int i;

    config->bind_ip = "0.0.0.0";
    config->sip_port = 5060;
    config->rtp_port = 40000;
    config->floor_timeout_seconds = 30;
    config->run_seconds = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            config->bind_ip = argv[++i];
        } else if (strcmp(argv[i], "--sip-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->sip_port)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--rtp-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->rtp_port)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--floor-timeout") == 0 && i + 1 < argc) {
            config->floor_timeout_seconds = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) {
            config->run_seconds = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            return 0;
        }
    }
    return 1;
}

static int make_udp_socket(const char *bind_ip, uint16_t port)
{
    int fd;
    int reuse = 1;
    int socket_buffer = 4 * 1024 * 1024;
    struct sockaddr_in address;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer, sizeof(socket_buffer));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_buffer, sizeof(socket_buffer));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static const char *address_text(const struct sockaddr_in *address,
                                char *buffer,
                                size_t buffer_size)
{
    const char *result = inet_ntop(AF_INET, &address->sin_addr, buffer, buffer_size);
    return result != NULL ? result : "?";
}

static int same_endpoint(const struct sockaddr_in *left,
                         const struct sockaddr_in *right)
{
    return left->sin_family == right->sin_family &&
           left->sin_port == right->sin_port &&
           left->sin_addr.s_addr == right->sin_addr.s_addr;
}

static int find_client_by_id(const char *id)
{
    int i;

    for (i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (g_clients[i].used && strcmp(g_clients[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_or_create_client(const char *id)
{
    int index = find_client_by_id(id);
    int i;

    if (index >= 0) {
        return index;
    }

    for (i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (!g_clients[i].used) {
            memset(&g_clients[i], 0, sizeof(g_clients[i]));
            g_clients[i].used = 1;
            strncpy(g_clients[i].id, id, sizeof(g_clients[i].id) - 1);
            return i;
        }
    }
    return -1;
}

static int find_floor(const char *group, int create)
{
    int i;

    for (i = 0; i < SERVER_MAX_FLOORS; ++i) {
        if (g_floors[i].used && strcmp(g_floors[i].group, group) == 0) {
            return i;
        }
    }

    if (!create) {
        return -1;
    }

    for (i = 0; i < SERVER_MAX_FLOORS; ++i) {
        if (!g_floors[i].used) {
            memset(&g_floors[i], 0, sizeof(g_floors[i]));
            g_floors[i].used = 1;
            g_floors[i].owner_index = -1;
            strncpy(g_floors[i].group, group, sizeof(g_floors[i].group) - 1);
            return i;
        }
    }
    return -1;
}

static void send_group_event(const char *group,
                             const char *speaker,
                             const char *event_body,
                             int excluded_client)
{
    int i;
    unsigned int notified = 0;

    if (g_sip_fd < 0) {
        return;
    }

    for (i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        char message[PTT_MAX_SIP_PACKET];
        char destination_ip[INET_ADDRSTRLEN];
        unsigned int cseq;
        int length;

        if (i == excluded_client || !g_clients[i].used || !g_clients[i].joined ||
            strcmp(g_clients[i].group, group) != 0) {
            continue;
        }

        cseq = g_server_cseq++;
        address_text(&g_clients[i].sip_address, destination_ip, sizeof(destination_ip));
        length = snprintf(message,
                          sizeof(message),
                          "MESSAGE sip:%s@%s:%u SIP/2.0\r\n"
                          "Via: SIP/2.0/UDP ptt-server;branch=z9hG4bK-server-%u\r\n"
                          "Max-Forwards: 70\r\n"
                          "From: <sip:ptt-server@server>;tag=server\r\n"
                          "To: <sip:%s@server>\r\n"
                          "Call-ID: event-%u@ptt-server\r\n"
                          "CSeq: %u MESSAGE\r\n"
                          "X-Group-ID: %s\r\n"
                          "X-PTT-Speaker: %s\r\n"
                          "Content-Type: application/x-ptt-event\r\n"
                          "Content-Length: %zu\r\n\r\n"
                          "%s",
                          g_clients[i].id,
                          destination_ip,
                          ntohs(g_clients[i].sip_address.sin_port),
                          cseq,
                          g_clients[i].id,
                          cseq,
                          cseq,
                          group,
                          speaker,
                          strlen(event_body),
                          event_body);
        if (length <= 0 || (size_t)length >= sizeof(message)) {
            continue;
        }

        if (sendto(g_sip_fd,
                   message,
                   (size_t)length,
                   0,
                   (const struct sockaddr *)&g_clients[i].sip_address,
                   sizeof(g_clients[i].sip_address)) >= 0) {
            ++notified;
        }
    }

    printf("[notify] group=%s event=%s speaker=%s listeners=%u\n",
           group,
           event_body,
           speaker,
           notified);
}

static void release_floors_owned_by(int client_index, const char *reason)
{
    int i;

    for (i = 0; i < SERVER_MAX_FLOORS; ++i) {
        if (g_floors[i].used && g_floors[i].owner_index == client_index) {
            send_group_event(g_floors[i].group,
                             g_clients[client_index].id,
                             "PTT-IDLE",
                             client_index);
            printf("[floor] group=%s owner=%s released (%s)\n",
                   g_floors[i].group,
                   g_clients[client_index].id,
                   reason);
            g_floors[i].owner_index = -1;
        }
    }
}

static void append_header_copy(char *output,
                               size_t output_size,
                               size_t *used,
                               const char *request,
                               const char *name)
{
    char value[512];
    int written;

    if (!ptt_get_header(request, name, value, sizeof(value)) || *used >= output_size) {
        return;
    }

    written = snprintf(output + *used, output_size - *used, "%s: %s\r\n", name, value);
    if (written > 0) {
        *used += (size_t)written < output_size - *used ? (size_t)written : output_size - *used;
    }
}

static void send_sip_response(int sip_fd,
                              const struct sockaddr_in *destination,
                              const char *request,
                              int status,
                              const char *reason,
                              const char *extra_headers,
                              const char *content_type,
                              const char *body)
{
    char response[PTT_MAX_SIP_PACKET];
    size_t used;
    size_t body_length = body != NULL ? strlen(body) : 0;
    int written;

    written = snprintf(response, sizeof(response), "SIP/2.0 %d %s\r\n", status, reason);
    if (written < 0) {
        return;
    }
    used = (size_t)written;

    append_header_copy(response, sizeof(response), &used, request, "Via");
    append_header_copy(response, sizeof(response), &used, request, "From");
    append_header_copy(response, sizeof(response), &used, request, "To");
    append_header_copy(response, sizeof(response), &used, request, "Call-ID");
    append_header_copy(response, sizeof(response), &used, request, "CSeq");

    if (extra_headers != NULL) {
        written = snprintf(response + used, sizeof(response) - used, "%s", extra_headers);
        if (written < 0 || (size_t)written >= sizeof(response) - used) {
            return;
        }
        used += (size_t)written;
    }

    if (content_type != NULL) {
        written = snprintf(response + used,
                           sizeof(response) - used,
                           "Content-Type: %s\r\n",
                           content_type);
        if (written < 0 || (size_t)written >= sizeof(response) - used) {
            return;
        }
        used += (size_t)written;
    }

    written = snprintf(response + used,
                       sizeof(response) - used,
                       "Content-Length: %zu\r\n\r\n%s",
                       body_length,
                       body != NULL ? body : "");
    if (written < 0 || (size_t)written >= sizeof(response) - used) {
        return;
    }
    used += (size_t)written;

    sendto(sip_fd,
           response,
           used,
           0,
           (const struct sockaddr *)destination,
           sizeof(*destination));
}

static int request_identity(const char *message,
                            char *id,
                            size_t id_size,
                            char *group,
                            size_t group_size)
{
    return ptt_get_header(message, "X-Device-ID", id, id_size) &&
           ptt_get_header(message, "X-Group-ID", group, group_size);
}

static void handle_register(int sip_fd,
                            const struct sockaddr_in *source,
                            const char *message)
{
    char id[PTT_MAX_ID];
    char group[PTT_MAX_GROUP];
    char value[128];
    int index;
    long rtp_port;
    unsigned long ssrc;
    unsigned long expires = 3600;
    int self_loopback = 0;
    char ip[INET_ADDRSTRLEN];

    if (!request_identity(message, id, sizeof(id), group, sizeof(group)) ||
        !ptt_get_header(message, "X-RTP-Port", value, sizeof(value))) {
        send_sip_response(sip_fd, source, message, 400, "Bad Request", NULL, NULL, NULL);
        return;
    }

    rtp_port = strtol(value, NULL, 10);
    if (rtp_port < 1 || rtp_port > 65535) {
        send_sip_response(sip_fd, source, message, 400, "Bad RTP Port", NULL, NULL, NULL);
        return;
    }

    if (ptt_get_header(message, "Expires", value, sizeof(value))) {
        expires = strtoul(value, NULL, 10);
    }
    ssrc = ptt_get_header(message, "X-SSRC", value, sizeof(value))
               ? strtoul(value, NULL, 10)
               : ptt_hash_text(id);
    if (ptt_get_header(message, "X-RTP-Loopback", value, sizeof(value))) {
        self_loopback = strtol(value, NULL, 10) != 0;
    }

    index = find_or_create_client(id);
    if (index < 0) {
        send_sip_response(sip_fd, source, message, 503, "Registry Full", NULL, NULL, NULL);
        return;
    }

    if (expires == 0) {
        release_floors_owned_by(index, "unregister");
        memset(&g_clients[index], 0, sizeof(g_clients[index]));
        send_sip_response(sip_fd, source, message, 200, "OK", NULL, NULL, NULL);
        return;
    }

    g_clients[index].registered = 1;
    g_clients[index].sip_address = *source;
    g_clients[index].rtp_address = *source;
    g_clients[index].rtp_address.sin_port = htons((uint16_t)rtp_port);
    g_clients[index].ssrc = (uint32_t)ssrc;
    g_clients[index].self_loopback = self_loopback;
    g_clients[index].expires_at_ms = ptt_now_ms() + expires * 1000u;
    strncpy(g_clients[index].group, group, sizeof(g_clients[index].group) - 1);

    printf("[sip] REGISTER id=%s group=%s sip=%s:%u rtp=%u ssrc=%u loopback=%d\n",
           id,
           group,
           address_text(source, ip, sizeof(ip)),
           ntohs(source->sin_port),
           (unsigned int)rtp_port,
           g_clients[index].ssrc,
           g_clients[index].self_loopback);
    send_sip_response(sip_fd, source, message, 200, "OK", "Expires: 3600\r\n", NULL, NULL);
}

static void handle_invite(int sip_fd,
                          const struct sockaddr_in *source,
                          const char *message,
                          uint16_t server_rtp_port)
{
    char id[PTT_MAX_ID];
    char group[PTT_MAX_GROUP];
    char extra[128];
    char sdp[512];
    int index;

    if (!request_identity(message, id, sizeof(id), group, sizeof(group))) {
        send_sip_response(sip_fd, source, message, 400, "Bad Request", NULL, NULL, NULL);
        return;
    }

    index = find_client_by_id(id);
    if (index < 0 || !g_clients[index].registered) {
        send_sip_response(sip_fd, source, message, 403, "Register First", NULL, NULL, NULL);
        return;
    }

    strncpy(g_clients[index].group, group, sizeof(g_clients[index].group) - 1);
    g_clients[index].joined = 1;
    snprintf(extra, sizeof(extra), "X-Server-RTP-Port: %u\r\n", server_rtp_port);
    snprintf(sdp,
             sizeof(sdp),
             "v=0\r\n"
             "o=ptt-server 1 1 IN IP4 0.0.0.0\r\n"
             "s=PTT group %s\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "t=0 0\r\n"
             "m=audio %u RTP/AVP %u\r\n"
             "a=rtpmap:%u opus/48000/1\r\n"
             "a=fmtp:%u minptime=5;useinbandfec=1\r\n"
             "a=ptime:20\r\n"
             "a=sendrecv\r\n",
             group,
             server_rtp_port,
             PTT_OPUS_PAYLOAD_TYPE,
             PTT_OPUS_PAYLOAD_TYPE,
             PTT_OPUS_PAYLOAD_TYPE);

    printf("[sip] INVITE/JOIN id=%s group=%s\n", id, group);
    send_sip_response(sip_fd, source, message, 200, "OK", extra, "application/sdp", sdp);
}

static void handle_ptt_message(int sip_fd,
                               const struct sockaddr_in *source,
                               const char *message)
{
    char id[PTT_MAX_ID];
    char group[PTT_MAX_GROUP];
    char extra[256];
    const char *body;
    int client_index;
    int floor_index;

    if (!request_identity(message, id, sizeof(id), group, sizeof(group))) {
        send_sip_response(sip_fd, source, message, 400, "Bad Request", NULL, NULL, NULL);
        return;
    }

    client_index = find_client_by_id(id);
    if (client_index < 0 || !g_clients[client_index].joined) {
        send_sip_response(sip_fd, source, message, 403, "Join First", NULL, NULL, NULL);
        return;
    }

    floor_index = find_floor(group, 1);
    if (floor_index < 0) {
        send_sip_response(sip_fd, source, message, 503, "Floor Table Full", NULL, NULL, NULL);
        return;
    }

    body = ptt_get_body(message);
    if (body != NULL && strncmp(body, "PTT-REQUEST", 11) == 0) {
        int owner = g_floors[floor_index].owner_index;

        if (owner < 0 || owner == client_index) {
            uint64_t now = ptt_now_ms();
            g_floors[floor_index].owner_index = client_index;
            g_floors[floor_index].granted_at_ms = now;
            g_floors[floor_index].last_rtp_ms = now;
            snprintf(extra, sizeof(extra), "X-PTT-Result: GRANTED\r\n");
            if (owner < 0) {
                send_group_event(group, id, "PTT-LISTEN", client_index);
            }
            printf("[floor] group=%s owner=%s GRANTED\n", group, id);
        } else {
            snprintf(extra,
                     sizeof(extra),
                     "X-PTT-Result: BUSY\r\nX-PTT-Owner: %s\r\n",
                     g_clients[owner].id);
            printf("[floor] group=%s requester=%s BUSY owner=%s\n",
                   group,
                   id,
                   g_clients[owner].id);
        }
        send_sip_response(sip_fd, source, message, 200, "OK", extra, NULL, NULL);
    } else if (body != NULL && strncmp(body, "PTT-RELEASE", 11) == 0) {
        if (g_floors[floor_index].owner_index == client_index) {
            send_group_event(group, id, "PTT-IDLE", client_index);
            g_floors[floor_index].owner_index = -1;
            snprintf(extra, sizeof(extra), "X-PTT-Result: RELEASED\r\n");
            printf("[floor] group=%s owner=%s RELEASED\n", group, id);
        } else {
            snprintf(extra, sizeof(extra), "X-PTT-Result: NOT-OWNER\r\n");
        }
        send_sip_response(sip_fd, source, message, 200, "OK", extra, NULL, NULL);
    } else {
        send_sip_response(sip_fd, source, message, 400, "Unknown PTT Command", NULL, NULL, NULL);
    }
}

static void handle_bye(int sip_fd,
                       const struct sockaddr_in *source,
                       const char *message)
{
    char id[PTT_MAX_ID];
    char group[PTT_MAX_GROUP];
    int index;

    if (!request_identity(message, id, sizeof(id), group, sizeof(group))) {
        send_sip_response(sip_fd, source, message, 400, "Bad Request", NULL, NULL, NULL);
        return;
    }

    index = find_client_by_id(id);
    if (index >= 0) {
        release_floors_owned_by(index, "BYE");
        g_clients[index].joined = 0;
        printf("[sip] BYE id=%s group=%s\n", id, group);
    }
    send_sip_response(sip_fd, source, message, 200, "OK", NULL, NULL, NULL);
}

static void handle_sip_packet(int sip_fd,
                              const struct sockaddr_in *source,
                              char *message,
                              uint16_t server_rtp_port)
{
    char method[32];

    if (sscanf(message, "%31s", method) != 1) {
        return;
    }

    if (strcmp(method, "SIP/2.0") == 0) {
        /* Response to a server-initiated PTT-LISTEN/PTT-IDLE notification. */
        return;
    }

    if (strcmp(method, "REGISTER") == 0) {
        handle_register(sip_fd, source, message);
    } else if (strcmp(method, "INVITE") == 0) {
        handle_invite(sip_fd, source, message, server_rtp_port);
    } else if (strcmp(method, "MESSAGE") == 0) {
        handle_ptt_message(sip_fd, source, message);
    } else if (strcmp(method, "BYE") == 0) {
        handle_bye(sip_fd, source, message);
    } else if (strcmp(method, "ACK") == 0) {
        /* ACK has no response. */
    } else if (strcmp(method, "OPTIONS") == 0) {
        send_sip_response(sip_fd, source, message, 200, "OK", NULL, NULL, NULL);
    } else {
        send_sip_response(sip_fd, source, message, 405, "Method Not Allowed", NULL, NULL, NULL);
    }
}

static int find_rtp_sender(const struct sockaddr_in *source, uint32_t ssrc)
{
    int i;

    for (i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (g_clients[i].used && g_clients[i].joined &&
            same_endpoint(&g_clients[i].rtp_address, source)) {
            return i;
        }
    }

    for (i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (g_clients[i].used && g_clients[i].joined && g_clients[i].ssrc == ssrc) {
            if (!same_endpoint(&g_clients[i].rtp_address, source)) {
                char ip[INET_ADDRSTRLEN];

                printf("[rtp] endpoint learned id=%s addr=%s:%u\n",
                       g_clients[i].id,
                       address_text(source, ip, sizeof(ip)),
                       ntohs(source->sin_port));
            }
            g_clients[i].rtp_address = *source;
            return i;
        }
    }
    return -1;
}

static void handle_rtp_packet(int rtp_fd,
                              const struct sockaddr_in *source,
                              const uint8_t *packet,
                              size_t packet_length)
{
    ptt_rtp_view view;
    int sender_index;
    int floor_index;
    int i;
    unsigned int recipients = 0;
    unsigned int packet_send_errors = 0;
    uint64_t receive_ms;
    uint64_t receive_monotonic_ms;
    uint16_t previous_sequence;
    uint16_t sequence_delta;
    int previous_valid;
    int rtp_continuous;
    uint32_t previous_source_frame_id;
    uint32_t source_frame_delta;
    int source_previous_valid;
    int source_continuous;
    uint64_t capture_gap_ms;
    int continuous;
    const char *sequence_status;

    if (!ptt_parse_rtp(packet, packet_length, &view)) {
        return;
    }
    sender_index = find_rtp_sender(source, view.ssrc);
    if (sender_index < 0) {
        return;
    }

    /*
     * A listener may be behind NAT and has not sent audio yet. Its RTP
     * keepalive updates g_clients[sender_index].rtp_address through the SSRC
     * match in find_rtp_sender(). Keepalives are deliberately not forwarded.
     */
    if (view.payload_type == PTT_RTP_KEEPALIVE_PAYLOAD_TYPE) {
        return;
    }
    if (view.payload_type != PTT_OPUS_PAYLOAD_TYPE) {
        return;
    }

    floor_index = find_floor(g_clients[sender_index].group, 0);
    if (floor_index < 0 || g_floors[floor_index].owner_index != sender_index) {
        return;
    }

    receive_ms = ptt_unix_time_ms();
    receive_monotonic_ms = ptt_now_ms();
    g_floors[floor_index].last_rtp_ms = receive_monotonic_ms;

    previous_valid = g_clients[sender_index].rx_sequence_valid && !view.marker;
    previous_sequence = g_clients[sender_index].rx_last_sequence;
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
    previous_source_frame_id =
        g_clients[sender_index].rx_last_source_frame_id;
    source_previous_valid = view.has_source_frame_id &&
                            g_clients[sender_index].rx_source_frame_valid &&
                            !view.marker;
    source_frame_delta = source_previous_valid
                             ? view.source_frame_id - previous_source_frame_id
                             : 0u;
    source_continuous = !source_previous_valid || source_frame_delta == 1u;
    capture_gap_ms = view.has_capture_time_ms &&
                             g_clients[sender_index].rx_capture_time_valid &&
                             !view.marker &&
                             view.capture_time_ms >=
                                 g_clients[sender_index].rx_last_capture_time_ms
                         ? view.capture_time_ms -
                               g_clients[sender_index].rx_last_capture_time_ms
                         : 0u;
    continuous = rtp_continuous && source_continuous;
    if (!previous_valid ||
        (sequence_delta > 0u && sequence_delta < 0x8000u)) {
        g_clients[sender_index].rx_sequence_valid = 1;
        g_clients[sender_index].rx_last_sequence = view.sequence;
    }
    if (view.has_source_frame_id &&
        (!source_previous_valid ||
         (source_frame_delta > 0u && source_frame_delta < 0x80000000u))) {
        g_clients[sender_index].rx_source_frame_valid = 1;
        g_clients[sender_index].rx_last_source_frame_id = view.source_frame_id;
    }
    if (view.has_capture_time_ms) {
        g_clients[sender_index].rx_capture_time_valid = 1;
        g_clients[sender_index].rx_last_capture_time_ms = view.capture_time_ms;
    }
    for (i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        int is_sender = i == sender_index;
        ssize_t sent;
        uint64_t transmit_ms;

        if ((is_sender && !g_clients[sender_index].self_loopback) ||
            !g_clients[i].used || !g_clients[i].joined ||
            strcmp(g_clients[i].group, g_clients[sender_index].group) != 0) {
            continue;
        }

        sent = sendto(rtp_fd,
                      packet,
                      packet_length,
                      0,
                      (const struct sockaddr *)&g_clients[i].rtp_address,
                      sizeof(g_clients[i].rtp_address));
        transmit_ms = ptt_unix_time_ms();
        if (sent >= 0) {
            ++recipients;
            ++g_rtp_stats_forwarded_copies;
        } else {
            ++g_rtp_stats_send_errors;
            ++packet_send_errors;
        }
        if (is_sender) {
            printf("[loopback-server] id=%s group=%s capture_frame=%u seq=%u "
                   "phone_send_ms=%llu server_rx_ms=%llu server_tx_ms=%llu "
                   "server_process_ms=%lld phone_to_server_clock_delta_ms=%lld "
                   "payload=%zu result=%s\n",
                   g_clients[sender_index].id,
                   g_clients[sender_index].group,
                   view.source_frame_id,
                   view.sequence,
                   (unsigned long long)(view.has_send_time_ms
                                            ? view.send_time_ms
                                            : 0u),
                   (unsigned long long)receive_ms,
                   (unsigned long long)transmit_ms,
                   (long long)((int64_t)transmit_ms - (int64_t)receive_ms),
                   (long long)(view.has_send_time_ms
                                   ? (int64_t)receive_ms -
                                         (int64_t)view.send_time_ms
                                   : -1),
                   view.payload_length,
                   sent >= 0 ? "sent" : "send_error");
        }
    }

    append_packet_history(&g_clients[sender_index],
                          &view,
                          receive_ms,
                          recipients,
                          packet_send_errors,
                          continuous,
                          sequence_status);

    printf("[frame-server-rx] group=%s from=%s capture_frame=%u "
           "has_capture_frame=%d source_previous=%u source_previous_valid=%d "
           "source_delta=%u source_continuous=%d capture_ms=%llu "
           "capture_gap_ms=%llu seq=%u previous=%u previous_valid=%d delta=%u "
           "rtp_continuous=%d continuous=%d marker=%u status=%s "
           "receive_ms=%llu send_ms=%llu age_ms=%lld payload=%zu recipients=%u\n",
           g_clients[sender_index].group,
           g_clients[sender_index].id,
           view.source_frame_id,
           view.has_source_frame_id,
           previous_source_frame_id,
           source_previous_valid,
           source_frame_delta,
           source_continuous,
           (unsigned long long)(view.has_capture_time_ms
                                    ? view.capture_time_ms
                                    : 0u),
           (unsigned long long)capture_gap_ms,
           view.sequence,
           previous_sequence,
           previous_valid,
           sequence_delta,
           rtp_continuous,
           continuous,
           view.marker,
           sequence_status,
           (unsigned long long)receive_ms,
           (unsigned long long)(view.has_send_time_ms ? view.send_time_ms : 0u),
           (long long)(view.has_send_time_ms
                           ? (int64_t)receive_ms - (int64_t)view.send_time_ms
                           : -1),
           view.payload_length,
           recipients);

    ++g_clients[sender_index].forwarded_packets;
    if (g_clients[sender_index].forwarded_packets == 1 ||
        g_clients[sender_index].forwarded_packets % PTT_OPUS_PACKETS_PER_SECOND == 0) {
        int64_t age_at_server_ms = view.has_send_time_ms
                                       ? (int64_t)ptt_unix_time_ms() -
                                             (int64_t)view.send_time_ms
                                       : -1;

        printf("[rtp] route=client->server->group group=%s from=%s seq=%u "
               "payload=%zu bytes recipients=%u age_at_server_ms=%lld total=%llu\n",
               g_clients[sender_index].group,
               g_clients[sender_index].id,
               view.sequence,
               view.payload_length,
               recipients,
               (long long)age_at_server_ms,
               (unsigned long long)g_clients[sender_index].forwarded_packets);
    }
}

static void log_rtp_stats(void)
{
    uint64_t now_ms = ptt_now_ms();

    if (g_rtp_stats_started_ms == 0) {
        g_rtp_stats_started_ms = now_ms;
        return;
    }
    if (now_ms - g_rtp_stats_started_ms < 1000u) {
        return;
    }
    if (g_rtp_stats_received_packets > 0 || g_rtp_stats_send_errors > 0) {
        printf("[rtp/1s] received=%llu forwarded_copies=%llu send_errors=%llu "
               "drain_max=%llu\n",
               (unsigned long long)g_rtp_stats_received_packets,
               (unsigned long long)g_rtp_stats_forwarded_copies,
               (unsigned long long)g_rtp_stats_send_errors,
               (unsigned long long)g_rtp_stats_max_drain_packets);
    }
    g_rtp_stats_started_ms = now_ms;
    g_rtp_stats_received_packets = 0;
    g_rtp_stats_forwarded_copies = 0;
    g_rtp_stats_send_errors = 0;
    g_rtp_stats_max_drain_packets = 0;
}

static void maintain_state(unsigned int floor_timeout_seconds)
{
    uint64_t now = ptt_now_ms();
    int i;

    for (i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (g_clients[i].used && g_clients[i].registered &&
            g_clients[i].expires_at_ms <= now) {
            release_floors_owned_by(i, "registration expired");
            printf("[sip] registration expired id=%s\n", g_clients[i].id);
            memset(&g_clients[i], 0, sizeof(g_clients[i]));
        }
    }

    for (i = 0; i < SERVER_MAX_FLOORS; ++i) {
        if (g_floors[i].used && g_floors[i].owner_index >= 0 &&
            floor_timeout_seconds > 0 &&
            now - g_floors[i].last_rtp_ms >=
                (uint64_t)floor_timeout_seconds * 1000u) {
            int owner = g_floors[i].owner_index;
            send_group_event(g_floors[i].group,
                             g_clients[owner].id,
                             "PTT-IDLE",
                             owner);
            printf("[floor] group=%s owner=%s force released (timeout)\n",
                   g_floors[i].group,
                   g_clients[owner].id);
            g_floors[i].owner_index = -1;
        }
    }
}

int main(int argc, char **argv)
{
    server_config config;
    int sip_fd;
    int rtp_fd;
    uint64_t started_at;

    if (!parse_arguments(argc, argv, &config)) {
        print_usage(argv[0]);
        return 2;
    }

    sip_fd = make_udp_socket(config.bind_ip, config.sip_port);
    rtp_fd = make_udp_socket(config.bind_ip, config.rtp_port);
    if (sip_fd < 0 || rtp_fd < 0) {
        fprintf(stderr, "Failed to bind UDP sockets: %s\n", strerror(errno));
        if (sip_fd >= 0) {
            close(sip_fd);
        }
        if (rtp_fd >= 0) {
            close(rtp_fd);
        }
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_sip_fd = sip_fd;
    started_at = ptt_now_ms();

    printf("PTT server version: %s\n", PTT_PRODUCT_VERSION);
    printf("PTT server started: SIP/UDP %s:%u, RTP/UDP %s:%u\n",
           config.bind_ip,
           config.sip_port,
           config.bind_ip,
           config.rtp_port);
    printf("Media topology: clients send RTP only to this server; the server "
           "copies accepted packets to other joined clients in the same group\n");

    while (g_running) {
        fd_set read_set;
        struct timeval timeout;
        int maximum_fd = sip_fd > rtp_fd ? sip_fd : rtp_fd;
        int selected;

        if (g_dump_packet_history) {
            g_dump_packet_history = 0;
            dump_packet_history();
        }

        if (config.run_seconds > 0 &&
            ptt_now_ms() - started_at >= (uint64_t)config.run_seconds * 1000u) {
            break;
        }

        FD_ZERO(&read_set);
        FD_SET(sip_fd, &read_set);
        FD_SET(rtp_fd, &read_set);
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        selected = select(maximum_fd + 1, &read_set, NULL, NULL, &timeout);
        if (selected < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "select failed: %s\n", strerror(errno));
            break;
        }

        if (FD_ISSET(sip_fd, &read_set)) {
            char message[PTT_MAX_SIP_PACKET];
            struct sockaddr_in source;
            socklen_t source_length = sizeof(source);
            ssize_t received = recvfrom(sip_fd,
                                        message,
                                        sizeof(message) - 1,
                                        0,
                                        (struct sockaddr *)&source,
                                        &source_length);
            if (received > 0) {
                message[received] = '\0';
                handle_sip_packet(sip_fd, &source, message, config.rtp_port);
            }
        }

        if (FD_ISSET(rtp_fd, &read_set)) {
            uint64_t drained_packets = 0;

            for (;;) {
                uint8_t packet[PTT_MAX_RTP_PACKET];
                struct sockaddr_in source;
                socklen_t source_length = sizeof(source);
                ssize_t received = recvfrom(rtp_fd,
                                            packet,
                                            sizeof(packet),
                                            MSG_DONTWAIT,
                                            (struct sockaddr *)&source,
                                            &source_length);
                if (received < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        break;
                    }
                    fprintf(stderr, "RTP recvfrom failed: %s\n", strerror(errno));
                    break;
                }
                if (received == 0) {
                    break;
                }
                ++drained_packets;
                ++g_rtp_stats_received_packets;
                handle_rtp_packet(rtp_fd, &source, packet, (size_t)received);
            }
            if (drained_packets > g_rtp_stats_max_drain_packets) {
                g_rtp_stats_max_drain_packets = drained_packets;
            }
        }

        maintain_state(config.floor_timeout_seconds);
        log_rtp_stats();
    }

    close(sip_fd);
    close(rtp_fd);
    printf("PTT server stopped\n");
    return 0;
}

