#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "q7_tcp_driver.h"

static uint16_t sequence_id = 0;
static int tcp_sock = -1;

int tcp_init_connection(const char *ip, uint16_t port) {
    struct sockaddr_in addr;

    if (tcp_sock != -1) {
        return 0;
    }

    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) {
        perror("[TCP] socket");
        tcp_sock = -1;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[TCP] Invalid IP address: %s\n", ip);
        close(tcp_sock);
        tcp_sock = -1;
        return -1;
    }

    if (connect(tcp_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[TCP] connect");
        close(tcp_sock);
        tcp_sock = -1;
        return -1;
    }

    printf("[TCP] Connected to %s:%u\n", ip, (unsigned)port);
    return 0;
}

void tcp_close_connection(void) {
    if (tcp_sock != -1) {
        close(tcp_sock);
        tcp_sock = -1;
    }
}

static ssize_t tcp_send_bytes(const uint8_t *data, size_t len) {
    size_t total_sent = 0;

    if (tcp_sock == -1) {
        fprintf(stderr, "[TCP] Socket not initialized.\n");
        return -1;
    }

    while (total_sent < len) {
        ssize_t n = send(tcp_sock, data + total_sent, len - total_sent, 0);
        if (n < 0) {
            perror("[TCP] send");
            return -1;
        }
        if (n == 0) break;
        total_sent += (size_t)n;
    }

    return (ssize_t)total_sent;
}

static ssize_t tcp_recv_bytes(uint8_t *buf, size_t len) {
    size_t total = 0;

    if (tcp_sock == -1) {
        fprintf(stderr, "[TCP] Socket not initialized.\n");
        return -1;
    }

    while (total < len) {
        ssize_t n = recv(tcp_sock, buf + total, len - total, 0);
        if (n < 0) {
            perror("[TCP] recv");
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "[TCP] Connection closed by remote.\n");
            return 0;
        }
        total += (size_t)n;
    }

    return (ssize_t)total;
}

static uint16_t calculate_checksum(uint8_t *data, int len) {
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

void send_data(PacketType type, uint16_t payload_length, uint8_t *data) {
    uint8_t packet[MAX_PACKET_SIZE];
    uint16_t frag_total = (payload_length == 0) ? 1 :
                          (payload_length + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;

    for (uint16_t frag_id = 0; frag_id < frag_total; frag_id++) {
        uint16_t offset     = frag_id * MAX_PAYLOAD_SIZE;
        uint16_t chunk_size = (payload_length - offset < MAX_PAYLOAD_SIZE) ?
                              (payload_length - offset) : MAX_PAYLOAD_SIZE;

        packet[IDX_TYPE]         = (uint8_t)type;
        packet[IDX_SEQ_MSB]      = (sequence_id >> 8) & 0xFF;
        packet[IDX_SEQ_LSB]      = sequence_id & 0xFF;
        packet[IDX_FRAG_ID_MSB]  = (frag_id >> 8) & 0xFF;
        packet[IDX_FRAG_ID_LSB]  = frag_id & 0xFF;
        packet[IDX_FRAG_TOT_MSB] = (frag_total >> 8) & 0xFF;
        packet[IDX_FRAG_TOT_LSB] = frag_total & 0xFF;
        packet[IDX_LEN_MSB]      = (chunk_size >> 8) & 0xFF;
        packet[IDX_LEN_LSB]      = chunk_size & 0xFF;
        packet[IDX_CRC_MSB]      = 0x00;
        packet[IDX_CRC_LSB]      = 0x00;

        if (chunk_size > 0 && data != NULL) {
            memcpy(&packet[IDX_DATA_START], &data[offset], chunk_size);
        }

        uint16_t total_len = HEADER_SIZE + chunk_size;
        uint16_t crc = calculate_checksum(packet, total_len);
        packet[IDX_CRC_MSB] = (crc >> 8) & 0xFF;
        packet[IDX_CRC_LSB] = crc & 0xFF;

        if (tcp_send_bytes(packet, total_len) < 0) {
            fprintf(stderr, "[TCP] Failed to send fragment %u/%u\n", frag_id + 1, frag_total);
            return;
        }
    }

    sequence_id++;
}

int receive_packet(RawPacket *pkt) {
    uint8_t header[HEADER_SIZE];

    if (tcp_recv_bytes(header, HEADER_SIZE) <= 0) {
        return -1;
    }

    pkt->type           = (PacketType)header[IDX_TYPE];
    pkt->seq_id         = ((uint16_t)header[IDX_SEQ_MSB]      << 8) | header[IDX_SEQ_LSB];
    pkt->frag_id        = ((uint16_t)header[IDX_FRAG_ID_MSB]  << 8) | header[IDX_FRAG_ID_LSB];
    pkt->frag_total     = ((uint16_t)header[IDX_FRAG_TOT_MSB] << 8) | header[IDX_FRAG_TOT_LSB];
    pkt->payload_length = ((uint16_t)header[IDX_LEN_MSB]      << 8) | header[IDX_LEN_LSB];

    uint16_t received_crc = ((uint16_t)header[IDX_CRC_MSB] << 8) | header[IDX_CRC_LSB];

    if (pkt->payload_length > MAX_PAYLOAD_SIZE) {
        fprintf(stderr, "[TCP] Invalid payload length: %u\n", pkt->payload_length);
        return -1;
    }

    if (pkt->payload_length > 0) {
        if (tcp_recv_bytes(pkt->data, pkt->payload_length) <= 0) {
            return -1;
        }
    }

    /* Verify checksum over the full raw packet with CRC bytes zeroed */
    uint8_t raw[MAX_PACKET_SIZE];
    memcpy(raw, header, HEADER_SIZE);
    memcpy(&raw[IDX_DATA_START], pkt->data, pkt->payload_length);
    raw[IDX_CRC_MSB] = 0x00;
    raw[IDX_CRC_LSB] = 0x00;

    uint16_t computed_crc = calculate_checksum(raw, HEADER_SIZE + pkt->payload_length);
    if (computed_crc != received_crc) {
        fprintf(stderr, "[TCP] Checksum mismatch: expected 0x%04X, got 0x%04X\n",
                computed_crc, received_crc);
        return -1;
    }

    return 0;
}
