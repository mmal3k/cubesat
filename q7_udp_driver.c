#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
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
static struct sockaddr_in gs_addr;
static int               gs_addr_known = 0;
static uint16_t          sequence_id   = 0;

/* NACK data filled by wait_for_response() */
#define MAX_NACK_FRAGS   ((MAX_PAYLOAD_SIZE - 2) / 3)   /* 80 */
static uint32_t nack_frags[MAX_NACK_FRAGS];
static uint16_t nack_count = 0;

/* Return codes for wait_for_response() */
#define WAIT_ACK     0
#define WAIT_NACK    1
#define WAIT_TIMEOUT (-1)

int udp_init(uint16_t local_port, uint16_t gs_port) {
    if (udp_sock != -1)
        return 0;

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("[UDP] socket");
        return -1;
    }

    int broadcast = 1;
    if (setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST,
                   &broadcast, sizeof(broadcast)) < 0) {
        perror("[UDP] setsockopt SO_BROADCAST");
        close(udp_sock);
        udp_sock = -1;
        return -1;
    }

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

    memset(&gs_addr, 0, sizeof(gs_addr));
    gs_addr.sin_family      = AF_INET;
    inet_pton(AF_INET, "255.255.255.255", &gs_addr.sin_addr);
    gs_addr.sin_port        = htons(gs_port);
    gs_addr_known           = 0;

    printf("[UDP] Bound to port %u, beaconing to broadcast:%u\n",
           local_port, gs_port);
    return 0;
}

void udp_close(void) {
    if (udp_sock != -1) {
        close(udp_sock);
        udp_sock      = -1;
        gs_addr_known = 0;
    }
}

int udp_get_fd(void) { return udp_sock; }

/* ── Checksums ──────────────────────────────────────────────────── */

static uint16_t calculate_checksum(const uint8_t *data, int len) {
    uint16_t sum = 0;
    for (int i = 0; i < len; i++)
        sum += data[i];
    return sum;
}

static uint32_t crc32_compute(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFF;
}

/* ── Fragment helpers ───────────────────────────────────────────── */

/* Compute total number of fragments for a given payload length */
static uint32_t frag_total_for(uint32_t len) {
    return (len == 0) ? 1 : (len + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
}

/* Build and send a single fragment */
static int send_one_fragment(PacketType type, uint32_t total_len, uint8_t *data,
                              uint16_t seq, uint32_t frag_id, uint32_t frag_total) {
    uint8_t  packet[MAX_PACKET_SIZE];
    uint32_t offset     = frag_id * MAX_PAYLOAD_SIZE;
    uint16_t chunk_size = (total_len - offset < MAX_PAYLOAD_SIZE)
                        ? (uint16_t)(total_len - offset) : MAX_PAYLOAD_SIZE;

    packet[IDX_TYPE]          = (uint8_t)type;
    packet[IDX_SEQ_MSB]       = (seq        >> 8) & 0xFF;
    packet[IDX_SEQ_LSB]       =  seq               & 0xFF;
    packet[IDX_FRAG_ID_MSB]   = (frag_id    >> 16) & 0xFF;
    packet[IDX_FRAG_ID_MID]   = (frag_id    >>  8) & 0xFF;
    packet[IDX_FRAG_ID_LSB]   =  frag_id            & 0xFF;
    packet[IDX_FRAG_TOT_MSB]  = (frag_total >> 16) & 0xFF;
    packet[IDX_FRAG_TOT_MID]  = (frag_total >>  8) & 0xFF;
    packet[IDX_FRAG_TOT_LSB]  =  frag_total         & 0xFF;
    packet[IDX_LEN_MSB]       = (chunk_size >>  8) & 0xFF;
    packet[IDX_LEN_LSB]       =  chunk_size         & 0xFF;
    packet[IDX_CRC_MSB]       = 0x00;
    packet[IDX_CRC_LSB]       = 0x00;

    if (chunk_size > 0 && data != NULL)
        memcpy(&packet[IDX_DATA_START], &data[offset], chunk_size);

    uint16_t total_pkt_len = HEADER_SIZE + chunk_size;
    uint16_t crc           = calculate_checksum(packet, total_pkt_len);
    packet[IDX_CRC_MSB] = (crc >> 8) & 0xFF;
    packet[IDX_CRC_LSB] =  crc        & 0xFF;

    if (sendto(udp_sock, packet, total_pkt_len, 0,
               (struct sockaddr *)&gs_addr, sizeof(gs_addr)) < 0) {
        fprintf(stderr, "[UDP] sendto failed frag %u/%u: %s\n",
                frag_id + 1, frag_total, strerror(errno));
        return -1;
    }
    return 0;
}

/* Send all fragments (frag 0 .. frag_total-1) */
static int send_fragments(PacketType type, uint32_t total_len,
                           uint8_t *data, uint16_t seq) {
    uint32_t ft = frag_total_for(total_len);
    for (uint32_t fid = 0; fid < ft; fid++) {
        if (send_one_fragment(type, total_len, data, seq, fid, ft) < 0)
            return -1;
        if (ft > 100 && (fid % 1000 == 0 || fid == ft - 1))
            printf("\r[UDP] Sending seq=%u: %u/%u frags", seq, fid + 1, ft);
    }
    if (ft > 100) printf("\n");
    return 0;
}

/* Send only the missing fragments listed in frag_ids[] */
static int send_fragments_selective(PacketType type, uint32_t total_len,
                                     uint8_t *data, uint16_t seq,
                                     uint32_t frag_total,
                                     uint32_t *frag_ids, uint16_t n) {
    for (uint16_t i = 0; i < n; i++)
        if (send_one_fragment(type, total_len, data, seq, frag_ids[i], frag_total) < 0)
            return -1;
    return 0;
}

/* Send fragments start_frag .. frag_total-1 (for resume) */
static int send_fragments_from(PacketType type, uint32_t total_len,
                                uint8_t *data, uint16_t seq, uint32_t start_frag) {
    uint32_t ft = frag_total_for(total_len);
    for (uint32_t fid = start_frag; fid < ft; fid++)
        if (send_one_fragment(type, total_len, data, seq, fid, ft) < 0)
            return -1;
    return 0;
}

/* ── Wait for ACK or NACK ───────────────────────────────────────── */

/*
 * Block up to ACK_TIMEOUT_MS waiting for a response to expected_seq.
 * Returns WAIT_ACK, WAIT_NACK (fills nack_frags/nack_count), or WAIT_TIMEOUT.
 */
static int wait_for_response(uint16_t expected_seq) {
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
        if (ms_left <= 0) return WAIT_TIMEOUT;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp_sock, &rfds);
        struct timeval tv = { .tv_sec  = ms_left / 1000,
                              .tv_usec = (ms_left % 1000) * 1000 };

        if (select(udp_sock + 1, &rfds, NULL, NULL, &tv) <= 0)
            return WAIT_TIMEOUT;

        RawPacket pkt;
        if (receive_packet(&pkt) < 0) continue;

        if (pkt.type == PID_ACK && pkt.seq_id == expected_seq)
            return WAIT_ACK;

        if (pkt.type == PID_NACK && pkt.seq_id == expected_seq) {
            /* Parse missing fragment list from payload:
             * [n_MSB][n_LSB][frag0_B2][frag0_B1][frag0_B0]... (3 bytes per frag) */
            if (pkt.payload_length < 2) continue;
            nack_count = ((uint16_t)pkt.data[0] << 8) | pkt.data[1];
            if (nack_count > MAX_NACK_FRAGS) nack_count = MAX_NACK_FRAGS;
            for (uint16_t i = 0; i < nack_count; i++)
                nack_frags[i] = ((uint32_t)pkt.data[2 + i*3] << 16)
                              | ((uint32_t)pkt.data[3 + i*3] <<  8)
                              |  (uint32_t)pkt.data[4 + i*3];
            return WAIT_NACK;
        }

        /* Any other packet — discard and keep waiting */
    }
}

/* ── Public API ─────────────────────────────────────────────────── */

void send_data(PacketType type, uint32_t payload_length, uint8_t *data) {
    int needs_ack = (type != PID_PING && type != PID_ACK && type != PID_ERROR);

    /* Append CRC32 of the full payload for end-to-end integrity */
    uint8_t  *send_buf = data;
    uint32_t  send_len = payload_length;
    uint8_t  *crc_buf  = NULL;

    if (payload_length > 0 && data != NULL) {
        crc_buf = malloc(payload_length + 4);
        if (crc_buf) {
            uint32_t crc = crc32_compute(data, payload_length);
            memcpy(crc_buf, data, payload_length);
            crc_buf[payload_length]     = (crc >> 24) & 0xFF;
            crc_buf[payload_length + 1] = (crc >> 16) & 0xFF;
            crc_buf[payload_length + 2] = (crc >>  8) & 0xFF;
            crc_buf[payload_length + 3] =  crc        & 0xFF;
            send_buf = crc_buf;
            send_len = payload_length + 4;
        }
    }

    uint32_t ft = frag_total_for(send_len);

    /* Send all fragments for the first attempt */
    if (send_fragments(type, send_len, send_buf, sequence_id) < 0)
        goto done;

    if (!needs_ack)
        goto done;

    int timeouts = 0;
    while (1) {
        int result = wait_for_response(sequence_id);

        if (result == WAIT_ACK) {
            printf("[UDP] ACK received for seq=%u\n", sequence_id);
            break;
        }

        if (result == WAIT_NACK) {
            /* GS told us exactly which fragments are missing — retransmit only those */
            printf("[UDP] NACK seq=%u — resending %u missing frags\n",
                   sequence_id, nack_count);
            send_fragments_selective(type, send_len, send_buf,
                                     sequence_id, ft, nack_frags, (uint16_t)nack_count);
            /* NACKs do not consume the retry budget — keep waiting */
            continue;
        }

        /* Timeout — full retransmit */
        timeouts++;
        if (timeouts > MAX_RETRIES) {
            fprintf(stderr, "[UDP] Gave up on seq=%u after %d timeouts\n",
                    sequence_id, MAX_RETRIES);
            break;
        }
        printf("[UDP] Timeout seq=%u — retransmitting all (timeout %d/%d)\n",
               sequence_id, timeouts, MAX_RETRIES);
        send_fragments(type, send_len, send_buf, sequence_id);
    }

done:
    free(crc_buf);
    sequence_id++;
}

/*
 * Resume a previously interrupted transfer.
 * Uses resume_seq as the seq_id (not the global counter) so the GS can
 * merge these fragments with the ones it saved from the previous pass.
 * Sends only fragments start_frag .. frag_total-1.
 */
void send_data_resume(PacketType type, uint32_t payload_length, uint8_t *data,
                      uint32_t start_frag, uint16_t resume_seq) {
    if (payload_length == 0 || data == NULL) return;

    /* Rebuild the same send_buf (data + CRC32) as the original send_data() call */
    uint8_t *crc_buf = malloc(payload_length + 4);
    if (!crc_buf) {
        fprintf(stderr, "[UDP] send_data_resume: malloc failed\n");
        return;
    }
    uint32_t crc = crc32_compute(data, payload_length);
    memcpy(crc_buf, data, payload_length);
    crc_buf[payload_length]     = (crc >> 24) & 0xFF;
    crc_buf[payload_length + 1] = (crc >> 16) & 0xFF;
    crc_buf[payload_length + 2] = (crc >>  8) & 0xFF;
    crc_buf[payload_length + 3] =  crc        & 0xFF;
    uint32_t send_len = payload_length + 4;

    uint32_t ft = frag_total_for(send_len);

    printf("[UDP] Resuming seq=%u from frag %u/%u\n",
           resume_seq, start_frag, ft);

    if (send_fragments_from(type, send_len, crc_buf, resume_seq, start_frag) < 0)
        goto done;

    int timeouts = 0;
    while (1) {
        int result = wait_for_response(resume_seq);

        if (result == WAIT_ACK) {
            printf("[UDP] ACK received for resumed seq=%u\n", resume_seq);
            break;
        }
        if (result == WAIT_NACK) {
            printf("[UDP] NACK (resume) seq=%u — resending %u frags\n",
                   resume_seq, nack_count);
            send_fragments_selective(type, send_len, crc_buf,
                                     resume_seq, ft, nack_frags, (uint16_t)nack_count);
            continue;
        }
        timeouts++;
        if (timeouts > MAX_RETRIES) {
            fprintf(stderr, "[UDP] Resume gave up on seq=%u after %d timeouts\n",
                    resume_seq, MAX_RETRIES);
            break;
        }
        send_fragments_from(type, send_len, crc_buf, resume_seq, start_frag);
    }

done:
    free(crc_buf);
    /* Note: sequence_id is NOT incremented — this is a resumed transfer */
}

int receive_packet(RawPacket *pkt) {
    uint8_t            buf[MAX_PACKET_SIZE];
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
    pkt->seq_id         = ((uint16_t)buf[IDX_SEQ_MSB]       << 8)  |  buf[IDX_SEQ_LSB];
    pkt->frag_id        = ((uint32_t)buf[IDX_FRAG_ID_MSB]   << 16) | ((uint32_t)buf[IDX_FRAG_ID_MID]  << 8) | buf[IDX_FRAG_ID_LSB];
    pkt->frag_total     = ((uint32_t)buf[IDX_FRAG_TOT_MSB]  << 16) | ((uint32_t)buf[IDX_FRAG_TOT_MID] << 8) | buf[IDX_FRAG_TOT_LSB];
    pkt->payload_length = ((uint16_t)buf[IDX_LEN_MSB]       << 8)  |  buf[IDX_LEN_LSB];

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

    buf[IDX_CRC_MSB] = 0x00;
    buf[IDX_CRC_LSB] = 0x00;
    uint16_t computed_crc = calculate_checksum(buf, HEADER_SIZE + pkt->payload_length);
    if (computed_crc != received_crc) {
        fprintf(stderr, "[UDP] Checksum mismatch: expected 0x%04X, got 0x%04X\n",
                computed_crc, received_crc);
        return -1;
    }

    if (!gs_addr_known) {
        gs_addr       = sender;
        gs_addr_known = 1;
        printf("[UDP] GS address learned: %s:%u — switching to unicast\n",
               inet_ntoa(sender.sin_addr), ntohs(sender.sin_port));
    }

    return 0;
}
