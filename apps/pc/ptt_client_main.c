#include "ptt_client.h"
#include "ptt_audio.h"
#include "ptt_pc_audio.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <unistd.h>

typedef struct {
    ptt_client_config native;
    unsigned int run_seconds;
    unsigned int auto_talk_delay_ms;
    unsigned int auto_talk_ms;
} cli_config;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int signal_number)
{
    (void)signal_number;
    g_running = 0;
}

static void console_log(void *user_data, const char *message)
{
    (void)user_data;
    puts(message);
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
            "  --opus-bitrate BPS         default 16000\n"
            "  --simulate-opus            send generated packets; no microphone\n",
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

static int parse_arguments(int argc, char **argv, cli_config *config)
{
    int i;

    memset(config, 0, sizeof(*config));
    ptt_client_config_init(&config->native);
    config->native.log_fn = console_log;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            strncpy(config->native.server_host,
                    argv[++i],
                    sizeof(config->native.server_host) - 1);
        } else if (strcmp(argv[i], "--server-sip-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->native.server_sip_port)) return 0;
        } else if (strcmp(argv[i], "--server-rtp-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->native.server_rtp_port)) return 0;
        } else if (strcmp(argv[i], "--sip-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->native.local_sip_port)) return 0;
        } else if (strcmp(argv[i], "--rtp-port") == 0 && i + 1 < argc) {
            if (!parse_port(argv[++i], &config->native.local_rtp_port)) return 0;
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            strncpy(config->native.device_id,
                    argv[++i],
                    sizeof(config->native.device_id) - 1);
        } else if (strcmp(argv[i], "--group") == 0 && i + 1 < argc) {
            strncpy(config->native.group_id,
                    argv[++i],
                    sizeof(config->native.group_id) - 1);
        } else if (strcmp(argv[i], "--run-seconds") == 0 && i + 1 < argc) {
            config->run_seconds = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--auto-talk-delay-ms") == 0 && i + 1 < argc) {
            config->auto_talk_delay_ms = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--auto-talk-ms") == 0 && i + 1 < argc) {
            config->auto_talk_ms = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--opus-bitrate") == 0 && i + 1 < argc) {
            config->native.opus_bitrate = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--simulate-opus") == 0) {
            config->native.simulate_opus = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            return 0;
        }
    }
    return config->native.device_id[0] != '\0' &&
           config->native.group_id[0] != '\0' &&
           config->native.local_sip_port != 0 &&
           config->native.local_rtp_port != 0 &&
           config->native.opus_bitrate >= 6000 &&
           config->native.opus_bitrate <= 128000;
}

static int stdin_ready(void)
{
    fd_set read_set;
    struct timeval timeout;

    FD_ZERO(&read_set);
    FD_SET(STDIN_FILENO, &read_set);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout) > 0;
}

static void print_status(const ptt_client_snapshot *snapshot)
{
    printf("status mode=%s speaker=%s registered=%d joined=%d floor=%d "
           "tx=%llu rx=%llu rejected_peer=%llu\n",
           ptt_client_mode_name(snapshot->mode),
           snapshot->speaker[0] != '\0' ? snapshot->speaker : "none",
           snapshot->registered,
           snapshot->joined,
           snapshot->floor_granted,
           (unsigned long long)snapshot->transmitted_packets,
           (unsigned long long)snapshot->received_packets,
           (unsigned long long)snapshot->rejected_peer_packets);
}

static void handle_command(ptt_client *client)
{
    char line[64];
    char command[32];
    ptt_client_snapshot snapshot;

    if (fgets(line, sizeof(line), stdin) == NULL || sscanf(line, "%31s", command) != 1) {
        return;
    }
    if (strcasecmp(command, "talk") == 0 || strcasecmp(command, "press") == 0 ||
        strcasecmp(command, "start") == 0) {
        if (ptt_client_request_talk(client) != 0) puts("cannot request floor");
    } else if (strcasecmp(command, "release") == 0 || strcasecmp(command, "stop") == 0) {
        (void)ptt_client_release_talk(client);
    } else if (strcasecmp(command, "status") == 0) {
        ptt_client_get_snapshot(client, &snapshot);
        print_status(&snapshot);
    } else if (strcasecmp(command, "quit") == 0 || strcasecmp(command, "exit") == 0) {
        g_running = 0;
    } else {
        puts("commands: talk, release, status, quit");
    }
}

int main(int argc, char **argv)
{
    cli_config config;
    ptt_client *client;
    ptt_pc_audio *audio = NULL;
    ptt_client_snapshot snapshot;
    uint64_t started_at_ms;
    uint64_t joined_at_ms = 0;
    uint64_t talking_at_ms = 0;
    int auto_requested = 0;
    int auto_released = 0;

    if (!parse_arguments(argc, argv, &config)) {
        print_usage(argv[0]);
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!config.native.simulate_opus) {
        audio = ptt_pc_audio_create(config.native.opus_bitrate);
        if (audio == NULL || ptt_pc_audio_start(audio) != 0) {
            fprintf(stderr, "Cannot start PC audio engine\n");
            ptt_pc_audio_destroy(audio);
            return 1;
        }
        config.native.opus_receive_fn = ptt_pc_audio_receive_opus;
        config.native.opus_receive_user_data = audio;
    }
    client = ptt_client_create(&config.native);
    if (client == NULL || ptt_client_start(client) != 0) {
        fprintf(stderr, "Cannot start PTT client\n");
        ptt_client_destroy(client);
        ptt_pc_audio_destroy(audio);
        return 1;
    }
    if (config.native.simulate_opus) {
        puts("[audio] simulation mode: generated Opus-sized payloads");
    } else {
        puts("[audio] real audio: capture 48000 Hz PCM16 mono -> 8000 Hz -> Opus; "
             "playback 8000 Hz PCM16 mono");
    }
    started_at_ms = ptt_now_ms();
    while (g_running) {
        uint64_t now;

        if (ptt_client_poll(client, 10) < 0) break;
        ptt_client_get_snapshot(client, &snapshot);
        if (audio != NULL) {
            uint8_t opus_packet[PTT_OPUS_MAX_PACKET_BYTES];
            size_t opus_length;
            uint32_t source_frame_id;
            uint64_t capture_time_ms;

            ptt_pc_audio_set_talking(audio, snapshot.mode == PTT_CLIENT_TALKING);
            while (ptt_pc_audio_take_opus(audio,
                                          opus_packet,
                                          sizeof(opus_packet),
                                           &opus_length,
                                           &source_frame_id,
                                           &capture_time_ms)) {
                (void)ptt_client_send_opus_frame(client,
                                                 opus_packet,
                                                 opus_length,
                                                 source_frame_id,
                                                 capture_time_ms,
                                                 NULL);
            }
        }
        now = ptt_now_ms();
        if (snapshot.joined && joined_at_ms == 0) joined_at_ms = now;
        if (snapshot.mode == PTT_CLIENT_TALKING && talking_at_ms == 0) talking_at_ms = now;
        if (config.auto_talk_ms > 0 && joined_at_ms > 0 && !auto_requested &&
            now - joined_at_ms >= config.auto_talk_delay_ms) {
            auto_requested = 1;
            (void)ptt_client_request_talk(client);
        }
        if (config.auto_talk_ms > 0 && talking_at_ms > 0 && !auto_released &&
            now - talking_at_ms >= config.auto_talk_ms) {
            auto_released = 1;
            (void)ptt_client_release_talk(client);
        }
        if (config.run_seconds > 0 &&
            now - started_at_ms >= (uint64_t)config.run_seconds * 1000u) {
            break;
        }
        if (config.run_seconds == 0 && stdin_ready()) handle_command(client);
    }
    ptt_pc_audio_set_talking(audio, 0);
    ptt_pc_audio_stop(audio);
    ptt_client_stop(client);
    ptt_client_get_snapshot(client, &snapshot);
    print_status(&snapshot);
    ptt_client_destroy(client);
    ptt_pc_audio_destroy(audio);
    return 0;
}
