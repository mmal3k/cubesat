#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "q7_tcp_driver.h"

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

int main(int argc, char *argv[]) {
    const char *ip       = "127.0.0.1";
    uint16_t    port     = 5000;
    const char *img_path = NULL;

    if (argc >= 2) ip       = argv[1];
    if (argc >= 3) port     = (uint16_t)atoi(argv[2]);
    if (argc >= 4) img_path = argv[3];

    if (tcp_init_connection(ip, port) != 0) {
        fprintf(stderr, "Failed to connect to %s:%u\n", ip, (unsigned)port);
        return 1;
    }

    /* Send a ping */
    printf("[TX] Sending PING\n");
    send_data(PID_PING, 0, NULL);

    /* Send a telemetry packet */
    uint8_t payload[] = "HELLO FROM Q7";
    printf("[TX] Sending TELEMETRY_HK: \"%s\"\n", payload);
    send_data(PID_TELEMETRY_HK, (uint16_t)(sizeof(payload) - 1), payload);

    /* Send image if path provided */
    if (img_path) {
        send_image(img_path);
    }

    /* Wait for packets from ground station */
    printf("[RX] Waiting for packets from ground station...\n");
    while (1) {
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
            printf("[TX] Sending ACK\n");
            send_data(PID_ACK, 0, NULL);
        }
    }

    tcp_close_connection();
    return 0;
}
