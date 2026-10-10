#ifndef PTT_CLIENT_H
#define PTT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "ptt_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PTT_CLIENT_STOPPED = 0,
    PTT_CLIENT_CONNECTING,
    PTT_CLIENT_REGISTERING,
    PTT_CLIENT_JOINING,
    PTT_CLIENT_IDLE,
    PTT_CLIENT_REQUESTING,
    PTT_CLIENT_TALKING,
    PTT_CLIENT_LISTENING,
    PTT_CLIENT_BUSY,
    PTT_CLIENT_ERROR
} ptt_client_mode;

typedef void (*ptt_client_log_fn)(void *user_data, const char *message);
typedef void (*ptt_client_opus_receive_fn)(void *user_data,
                                           const uint8_t *payload,
                                           size_t payload_length,
                                           uint16_t sequence);

typedef struct {
    char server_host[256];
    uint16_t server_sip_port;
    uint16_t server_rtp_port;
    uint16_t local_sip_port;
    uint16_t local_rtp_port;
    char device_id[PTT_MAX_ID];
    char group_id[PTT_MAX_GROUP];
    unsigned int opus_bitrate;
    int simulate_opus;
    ptt_client_log_fn log_fn;
    void *log_user_data;
    ptt_client_opus_receive_fn opus_receive_fn;
    void *opus_receive_user_data;
} ptt_client_config;

typedef struct {
    ptt_client_mode mode;
    int registered;
    int joined;
    int floor_granted;
    char speaker[PTT_MAX_ID];
    char detail[160];
    uint16_t local_sip_port;
    uint16_t local_rtp_port;
    uint64_t transmitted_packets;
    uint64_t received_packets;
    uint64_t rejected_peer_packets;
    uint64_t last_rx_packets_per_second;
    uint64_t last_rx_payload_bytes;
    double last_latency_average_ms;
    int64_t last_latency_min_ms;
    int64_t last_latency_max_ms;
} ptt_client_snapshot;

typedef struct ptt_client ptt_client;

void ptt_client_config_init(ptt_client_config *config);

ptt_client *ptt_client_create(const ptt_client_config *config);

/* Resolves the server, binds UDP sockets and starts REGISTER/INVITE. */
int ptt_client_start(ptt_client *client);

/* Processes SIP/RTP and timers. Returns -1 on fatal error, otherwise 0. */
int ptt_client_poll(ptt_client *client, unsigned int timeout_ms);

/* Asynchronous floor operations. The final result is reflected in snapshot.mode. */
int ptt_client_request_talk(ptt_client *client);
int ptt_client_release_talk(ptt_client *client);

/* Future real-audio entry point. Data must be one already encoded Opus packet. */
int ptt_client_send_opus(ptt_client *client,
                         const uint8_t *opus_packet,
                         size_t opus_packet_length);

void ptt_client_get_snapshot(const ptt_client *client,
                             ptt_client_snapshot *snapshot);

void ptt_client_stop(ptt_client *client);
void ptt_client_destroy(ptt_client *client);

const char *ptt_client_mode_name(ptt_client_mode mode);

#ifdef __cplusplus
}
#endif

#endif
