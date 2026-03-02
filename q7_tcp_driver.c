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

#define MAX_PAYLOAD_SIZE    (MAX_PACKET_SIZE - HEADER_SIZE)

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
        fprintf(stderr, "[TCP] Socket not initialized. Call tcp_init_connection() first.\n");
        return -1;
    }

    while (total_sent < len) {
        ssize_t n = send(tcp_sock, data + total_sent, len - total_sent, 0);
        if (n < 0) {
            perror("[TCP] send");
            return -1;
        }
        if (n == 0) {
            break;
        }
        total_sent += (size_t)n;
    }

    return (ssize_t)total_sent;
}

static uint16_t calculate_crc16(uint8_t *data, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc += data[i];
    }
    return crc;
}

void send_data(PacketType type, uint16_t payload_length, uint8_t *data) {
    uint8_t packet[MAX_PACKET_SIZE];
    uint16_t frag_total = (payload_length + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;

    packet[IDX_TYPE] = (uint8_t)type;

    packet[IDX_FRAG_TOT_LSB] = (frag_total >> 8) & 0xFF;
    packet[IDX_FRAG_TOT_MSB] = (frag_total) & 0xFF;

    uint32_t offset = 0;

    for (uint16_t frag_id = 0; frag_id <= frag_total; frag_id++) {
        uint16_t current_chunk_size = MAX_PAYLOAD_SIZE;

        if ((payload_length - offset) < MAX_PAYLOAD_SIZE) {
            current_chunk_size = (uint16_t)(payload_length - offset);
        }

        packet[IDX_SEQ_LSB] = (sequence_id >> 8) & 0xFF;
        packet[IDX_SEQ_MSB] = sequence_id & 0xFF;

        packet[IDX_FRAG_ID_LSB] = (frag_id >> 8) & 0xFF;
        packet[IDX_FRAG_ID_MSB] = (frag_id) & 0xFF;

        packet[IDX_LEN_MSB] = (current_chunk_size >> 8) & 0xFF;
        packet[IDX_LEN_LSB] = (current_chunk_size) & 0xFF;

        if (current_chunk_size > 0 && data != NULL) {
            memcpy(&packet[HEADER_SIZE], &data[offset], current_chunk_size);
        }

        packet[IDX_CRC_MSB] = 0x00;
        packet[IDX_CRC_LSB] = 0x00;

        uint16_t total_packet_len = HEADER_SIZE + current_chunk_size;
        uint16_t my_crc = calculate_crc16(packet, total_packet_len);

        packet[IDX_CRC_MSB] = (my_crc >> 8) & 0xFF;
        packet[IDX_CRC_LSB] = (my_crc) & 0xFF;

        if (tcp_send_bytes(packet, total_packet_len) < 0) {
            fprintf(stderr, "[TCP] Failed to send packet fragment %u\n", frag_id);
            return;
        }

        offset += current_chunk_size;
        sequence_id++;
    }
}

