#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "q7_udp_driver.h"

static int               udp_sock    = -1;
static struct sockaddr_in gs_addr;           /* destination: broadcast until GS is known */
static int               gs_addr_known = 0;
static uint16_t          sequence_id   = 0;

int udp_init(uint16_t local_port, uint16_t gs_port) {
    if (udp_sock != -1)
        return 0;

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("[UDP] socket");
        return -1;
    }

    /* Enable broadcast so we can send beacons without knowing the GS address */
    int broadcast = 1;
    if (setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST,
                   &broadcast, sizeof(broadcast)) < 0) {
        perror("[UDP] setsockopt SO_BROADCAST");
        close(udp_sock);
        udp_sock = -1;
        return -1;
    }

    /* Bind to local_port so the GS can send commands back to us */
    struct sockaddr_in local = {0};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons(local_port);

    if (bind(udp_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("[UDP] bind");
        close(udp_sock);
        udp_sock = -1;
        return -1;
    }

    /* Point gs_addr at the broadcast address until we hear from the GS */
    memset(&gs_addr, 0, sizeof(gs_addr));
    gs_addr.sin_family      = AF_INET;
    gs_addr.sin_addr.s_addr = INADDR_BROADCAST;
    gs_addr.sin_port        = htons(gs_port);
    gs_addr_known           = 0;

    printf("[UDP] Bound to port %u, beaconing to broadcast:%u\n",
           local_port, gs_port);
    return 0;
}

int udp_get_fd(void) { return udp_sock; }

void udp_close(void) {
    if (udp_sock != -1) {
        close(udp_sock);
        udp_sock    = -1;
        gs_addr_known = 0;
    }
}

static uint16_t calculate_checksum(const uint8_t *data, int len) {
    uint16_t sum = 0;
    for (int i = 0; i < len; i++)
        sum += data[i];
    return sum;
}

/* Send all fragments for a given sequence ID (no ACK logic here) */
static int send_fragments(PacketType type, uint32_t payload_length,
                           uint8_t *data, uint16_t seq) {
    uint8_t  packet[MAX_PACKET_SIZE];
    uint16_t frag_total = (payload_length == 0) ? 1 :
                          (uint16_t)((payload_length + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE);

    for (uint16_t frag_id = 0; frag_id < frag_total; frag_id++) {
        uint32_t offset     = (uint32_t)frag_id * MAX_PAYLOAD_SIZE;
        uint16_t chunk_size = (payload_length - offset < MAX_PAYLOAD_SIZE) ?
                              (uint16_t)(payload_length - offset) : MAX_PAYLOAD_SIZE;

        packet[IDX_TYPE]         = (uint8_t)type;
        packet[IDX_SEQ_MSB]      = (seq      >> 8) & 0xFF;
        packet[IDX_SEQ_LSB]      =  seq             & 0xFF;
        packet[IDX_FRAG_ID_MSB]  = (frag_id  >> 8) & 0xFF;
        packet[IDX_FRAG_ID_LSB]  =  frag_id         & 0xFF;
        packet[IDX_FRAG_TOT_MSB] = (frag_total >> 8) & 0xFF;
        packet[IDX_FRAG_TOT_LSB] =  frag_total       & 0xFF;
        packet[IDX_LEN_MSB]      = (chunk_size >> 8) & 0xFF;
        packet[IDX_LEN_LSB]      =  chunk_size        & 0xFF;
        packet[IDX_CRC_MSB]      = 0x00;
        packet[IDX_CRC_LSB]      = 0x00;

        if (chunk_size > 0 && data != NULL)
            memcpy(&packet[IDX_DATA_START], &data[offset], chunk_size);

        uint16_t total_len = HEADER_SIZE + chunk_size;
        uint16_t crc       = calculate_checksum(packet, total_len);
        packet[IDX_CRC_MSB] = (crc >> 8) & 0xFF;
        packet[IDX_CRC_LSB] =  crc        & 0xFF;

        if (sendto(udp_sock, packet, total_len, 0,
                   (struct sockaddr *)&gs_addr, sizeof(gs_addr)) < 0) {
            fprintf(stderr, "[UDP] sendto failed on fragment %u/%u: %s\n",
                    frag_id + 1, frag_total, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/*
 * Wait up to ACK_TIMEOUT_MS for a PID_ACK with matching seq_id.
 * Any non-ACK or wrong-seq packets received while waiting are discarded.
 * Returns 0 on success, -1 on timeout.
 */
static int wait_for_ack(uint16_t expected_seq) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec  += ACK_TIMEOUT_MS / 1000;
    deadline.tv_nsec += (ACK_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        long ms_left = (deadline.tv_sec  - now.tv_sec)  * 1000 +
                       (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (ms_left <= 0) return -1;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp_sock, &rfds);
        struct timeval tv = { .tv_sec  = ms_left / 1000,
                              .tv_usec = (ms_left % 1000) * 1000 };

        if (select(udp_sock + 1, &rfds, NULL, NULL, &tv) <= 0)
            return -1;  /* timeout */

        RawPacket pkt;
        if (receive_packet(&pkt) < 0) continue;

        if (pkt.type == PID_ACK && pkt.seq_id == expected_seq)
            return 0;  /* correct ACK received */

        /* Wrong type or seq — discard and keep waiting */
    }
}

void send_data(PacketType type, uint32_t payload_length, uint8_t *data) {
    /* PID_PING is a beacon (fire and forget). PID_ACK/ERROR never need an ACK. */
    int needs_ack = (type != PID_PING && type != PID_ACK && type != PID_ERROR);

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        if (attempt > 0)
            printf("[UDP] No ACK for seq=%u — retransmitting (attempt %d/%d)\n",
                   sequence_id, attempt, MAX_RETRIES);

        if (send_fragments(type, payload_length, data, sequence_id) < 0)
            break;

        if (!needs_ack)
            break;

        if (wait_for_ack(sequence_id) == 0) {
            printf("[UDP] ACK received for seq=%u\n", sequence_id);
            break;
        }

        if (attempt == MAX_RETRIES)
            fprintf(stderr, "[UDP] Gave up on seq=%u after %d attempts\n",
                    sequence_id, MAX_RETRIES);
    }

    sequence_id++;
}

int receive_packet(RawPacket *pkt) {
    uint8_t           buf[MAX_PACKET_SIZE];
    struct sockaddr_in sender;
    socklen_t          sender_len = sizeof(sender);

    ssize_t n = recvfrom(udp_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&sender, &sender_len);
    if (n < 0) {
        perror("[UDP] recvfrom");
        return -1;
    }
    if (n < HEADER_SIZE) {
        fprintf(stderr, "[UDP] Datagram too short: %zd bytes\n", n);
        return -1;
    }

    pkt->type           = (PacketType)buf[IDX_TYPE];
    pkt->seq_id         = ((uint16_t)buf[IDX_SEQ_MSB]      << 8) | buf[IDX_SEQ_LSB];
    pkt->frag_id        = ((uint16_t)buf[IDX_FRAG_ID_MSB]  << 8) | buf[IDX_FRAG_ID_LSB];
    pkt->frag_total     = ((uint16_t)buf[IDX_FRAG_TOT_MSB] << 8) | buf[IDX_FRAG_TOT_LSB];
    pkt->payload_length = ((uint16_t)buf[IDX_LEN_MSB]      << 8) | buf[IDX_LEN_LSB];

    uint16_t received_crc = ((uint16_t)buf[IDX_CRC_MSB] << 8) | buf[IDX_CRC_LSB];

    if (pkt->payload_length > MAX_PAYLOAD_SIZE) {
        fprintf(stderr, "[UDP] Invalid payload length: %u\n", pkt->payload_length);
        return -1;
    }

    if ((ssize_t)(HEADER_SIZE + pkt->payload_length) > n) {
        fprintf(stderr, "[UDP] Datagram shorter than declared payload\n");
        return -1;
    }

    if (pkt->payload_length > 0)
        memcpy(pkt->data, &buf[IDX_DATA_START], pkt->payload_length);

    /* Verify checksum */
    buf[IDX_CRC_MSB] = 0x00;
    buf[IDX_CRC_LSB] = 0x00;
    uint16_t computed_crc = calculate_checksum(buf, HEADER_SIZE + pkt->payload_length);
    if (computed_crc != received_crc) {
        fprintf(stderr, "[UDP] Checksum mismatch: expected 0x%04X, got 0x%04X\n",
                computed_crc, received_crc);
        return -1;
    }

    /*
     * Learn the GS address from the first incoming packet.
     * After this, send_data() sends unicast instead of broadcast.
     */
    if (!gs_addr_known) {
        gs_addr       = sender;
        gs_addr_known = 1;
        printf("[UDP] GS address learned: %s:%u — switching to unicast\n",
               inet_ntoa(sender.sin_addr), ntohs(sender.sin_port));
    }

    return 0;
}
