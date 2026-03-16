#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h>

#include "q7_udp_driver.h"

#define BEACON_INTERVAL_S  10   /* send PING + TELEMETRY_HK every N seconds */

static int send_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[IMG] Cannot open file: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size <= 0) {
        fprintf(stderr, "[IMG] File is empty\n");
        fclose(f);
        return -1;
    }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) {
        fprintf(stderr, "[IMG] malloc failed\n");
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        fprintf(stderr, "[IMG] Read error\n");
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    printf("[TX] Sending image: %s (%ld bytes)\n", path, file_size);
    send_data(PID_SCI_IMG, (uint32_t)file_size, buf);
    free(buf);
    return 0;
}

static const char *packet_type_name(PacketType type) {
    switch (type) {
        case PID_CMD_CONTROL:  return "CMD_CONTROL";
        case PID_PING:         return "PING";
        case PID_TELEMETRY_HK: return "TELEMETRY_HK";
        case PID_SCI_IMG:      return "SCI_IMG";
        case PID_SCI_TXT:      return "SCI_TXT";
        case PID_ACK:          return "ACK";
        case PID_ERROR:        return "ERROR";
        default:               return "UNKNOWN";
    }
}

static void send_beacon(void) {
    uint8_t payload[] = "HELLO FROM Q7";
    printf("[TX] Beacon: PING + TELEMETRY_HK\n");
    send_data(PID_PING, 0, NULL);
    send_data(PID_TELEMETRY_HK, (uint32_t)(sizeof(payload) - 1), payload);
}

int main(int argc, char *argv[]) {
    uint16_t    local_port = 5001;   /* Q7 listens here for commands    */
    uint16_t    gs_port    = 5000;   /* GS listens here for beacons     */
    const char *img_path   = NULL;

    if (argc >= 2) local_port = (uint16_t)atoi(argv[1]);
    if (argc >= 3) gs_port    = (uint16_t)atoi(argv[2]);
    if (argc >= 4) img_path   = argv[3];

    if (udp_init(local_port, gs_port) != 0) {
        fprintf(stderr, "Failed to init UDP (local=%u gs=%u)\n",
                local_port, gs_port);
        return 1;
    }

    /* Initial beacon */
    send_beacon();

    /* Send image if path provided */
    if (img_path) {
        send_image(img_path);
    }

    printf("[Q7] Beacon interval: %d s  |  Ctrl-C to stop\n", BEACON_INTERVAL_S);

    int fd = udp_get_fd();

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv = { .tv_sec = BEACON_INTERVAL_S, .tv_usec = 0 };
        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);

        if (ret < 0) {
            perror("[Q7] select");
            break;
        }

        if (ret == 0) {
            /* Timeout — send next beacon */
            send_beacon();
            continue;
        }

        /* Packet available — receive and handle */
        RawPacket pkt;
        if (receive_packet(&pkt) < 0) {
            fprintf(stderr, "[RX] Failed to receive packet, exiting.\n");
            break;
        }

        printf("[RX] [%s] seq=%u frag=%u/%u len=%u\n",
               packet_type_name(pkt.type),
               pkt.seq_id, pkt.frag_id + 1, pkt.frag_total,
               pkt.payload_length);

        if (pkt.payload_length > 0) {
            printf("     Payload: %.*s\n", (int)pkt.payload_length, pkt.data);
        }

        /* ACK any command we receive */
        if (pkt.type == PID_CMD_CONTROL) {
            printf("[TX] Sending ACK for seq=%u\n", pkt.seq_id);
            send_data(PID_ACK, 0, NULL);
        }
    }

    udp_close();
    return 0;
}
