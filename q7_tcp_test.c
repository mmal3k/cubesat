#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>

#include "q7_tcp_driver.h"

#define RECONNECT_MAX_ATTEMPTS  -1   /* -1 = retry forever */
#define RECONNECT_BASE_DELAY_S   2

/* ------------------------------------------------------------------ */
/*  File helpers                                                        */
/*  All file I/O belongs here in the application layer, not the driver.*/
/* ------------------------------------------------------------------ */

/*
 * send_file_list – enumerate dir_path and transmit as PID_SCI_TXT.
 *
 * Uses a dynamically grown buffer: starts at 4 KiB, doubles when full.
 * No fixed-size overflow risk regardless of directory size.
 */
static void send_file_list(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "[FILES] Cannot open directory: %s\n", dir_path);
        return;
    }

    size_t  cap  = 4096;
    size_t  used = 0;
    char   *buf  = (char *)malloc(cap);
    if (!buf) {
        fprintf(stderr, "[FILES] malloc failed\n");
        closedir(dir);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        size_t needed   = used + name_len + 2; /* '\n' + '\0' */

        if (needed > cap) {
            size_t new_cap = cap * 2;
            while (new_cap < needed) new_cap *= 2;
            char *tmp = (char *)realloc(buf, new_cap);
            if (!tmp) {
                fprintf(stderr, "[FILES] realloc failed – list truncated\n");
                break;
            }
            buf = tmp;
            cap = new_cap;
        }

        memcpy(buf + used, entry->d_name, name_len);
        used += name_len;
        buf[used++] = '\n';
    }
    buf[used] = '\0';
    closedir(dir);

    printf("[TX] Sending file list (%zu bytes)\n", used);
    send_data(PID_SCI_TXT, (uint32_t)used, (uint8_t *)buf);
    free(buf);
}

/*
 * send_file – read any binary file and transmit it.
 * 'type' lets callers choose PID_SCI_IMG or any other downlink type.
 */
static int send_file(const char *path, PacketType type)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[FILE] Cannot open: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size <= 0) {
        fprintf(stderr, "[FILE] Empty or unreadable: %s\n", path);
        fclose(f);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)file_size);
    if (!buf) {
        fprintf(stderr, "[FILE] malloc failed\n");
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        fprintf(stderr, "[FILE] Read error: %s\n", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    printf("[TX] Sending %s (%ld bytes)\n", path, file_size);
    send_data(type, (uint32_t)file_size, buf);
    free(buf);
    return 0;
}

static void send_latest_image(void)
{
    /* In production, scan for the newest file by timestamp.
     * For testing, we use a fixed well-known name. */
    send_file("latest_image.bin", PID_SCI_IMG);
}

/* ------------------------------------------------------------------ */
/*  Logging helper                                                      */
/* ------------------------------------------------------------------ */

static const char *packet_type_name(PacketType type)
{
    switch (type) {
        case PID_CMD_CONTROL:    return "CMD_CONTROL";
        case PID_PING:           return "PING";
        case PID_TELEMETRY_HK:   return "TELEMETRY_HK";
        case PID_SCI_IMG:        return "SCI_IMG";
        case PID_SCI_TXT:        return "SCI_TXT";
        case PID_ACK:            return "ACK";
        case PID_ERROR:          return "ERROR";
        case PID_LIST_FILES:     return "LIST_FILES";
        case PID_LATEST_IMG:     return "LATEST_IMG";
        case PID_GET_FILE:       return "GET_FILE";
        case PID_RETRANSMIT_REQ: return "RETRANSMIT_REQ";
        default:                 return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char *ip       = "127.0.0.1";
    uint16_t    port     = 5000;
    const char *img_path = NULL;

    if (argc >= 2) ip       = argv[1];
    if (argc >= 3) port     = (uint16_t)atoi(argv[2]);
    if (argc >= 4) img_path = argv[3];

    if (tcp_init_connection(ip, port) != 0) {
        fprintf(stderr, "[Q7] Failed to connect to %s:%u\n", ip, (unsigned)port);
        return 1;
    }

    /* Initial telemetry burst */
    printf("[TX] Sending PING\n");
    send_data(PID_PING, 0, NULL);

    uint8_t hk[] = "HELLO FROM Q7";
    printf("[TX] Sending TELEMETRY_HK: \"%s\"\n", hk);
    send_data(PID_TELEMETRY_HK, (uint32_t)(sizeof(hk) - 1), hk);

    if (img_path) send_file(img_path, PID_SCI_IMG);

    /* ---------------------------------------------------------------- */
    /*  Main receive / reconnect loop                                    */
    /* ---------------------------------------------------------------- */
    printf("[RX] Waiting for packets from ground station...\n");

    while (1) {
        RawPacket pkt;

        if (receive_packet(&pkt) < 0) {
            fprintf(stderr, "\n[Q7] Link down – attempting to reconnect...\n");
            if (tcp_reconnect(ip, port,
                              RECONNECT_MAX_ATTEMPTS,
                              RECONNECT_BASE_DELAY_S) != 0) {
                fprintf(stderr, "[Q7] Could not reconnect. Exiting.\n");
                return 1;
            }
            printf("[Q7] Reconnected – waiting for retransmit request...\n");
            continue;
        }

        printf("[RX] [%s] seq=%u frag=%u/%u len=%u\n",
               packet_type_name(pkt.type),
               pkt.seq_id, pkt.frag_id + 1, pkt.frag_total,
               pkt.payload_length);

        /* Print payload as text for non-binary types */
        if (pkt.payload_length > 0 &&
            pkt.type != PID_SCI_IMG &&
            pkt.type != PID_GET_FILE) {
            printf("     Payload: %.*s\n",
                   (int)pkt.payload_length, pkt.data);
        }

        switch (pkt.type) {

        case PID_CMD_CONTROL:
            printf("[TX] ACKing command\n");
            send_data(PID_ACK, 0, NULL);
            break;

        case PID_PING:
            send_data(PID_ACK, 0, NULL);
            break;

        case PID_LIST_FILES:
            printf("[CMD] Ground station requested file list\n");
            send_file_list(".");
            break;

        case PID_LATEST_IMG:
            printf("[CMD] Ground station requested latest image\n");
            send_latest_image();
            break;

        case PID_GET_FILE: {
            /* payload_length is capped at MAX_PAYLOAD_SIZE (245) by receive_packet() */
            if (pkt.payload_length == 0 ||
                pkt.payload_length >= MAX_PAYLOAD_SIZE) {
                fprintf(stderr, "[CMD] Invalid filename length: %u\n",
                        pkt.payload_length);
                break;
            }
            char path[MAX_PAYLOAD_SIZE + 1];
            memcpy(path, pkt.data, pkt.payload_length);
            path[pkt.payload_length] = '\0';

            /* Reject path traversal */
            if (strstr(path, "..") != NULL) {
                fprintf(stderr, "[CMD] Path traversal rejected: %s\n", path);
                break;
            }

            printf("[CMD] Ground station requested file: %s\n", path);
            send_file(path, PID_SCI_IMG);
            break;
        }

        case PID_RETRANSMIT_REQ:
            if (pkt.payload_length >= 2) {
                uint16_t last_good = ((uint16_t)pkt.data[0] << 8)
                                     | pkt.data[1];
                printf("[RX] Retransmit requested from seq > %u\n", last_good);
                tcp_retransmit_from(last_good);
            } else {
                printf("[RX] Full retransmit requested\n");
                tcp_retransmit_from(0xFFFF);
            }
            break;

        default:
            break;
        }
    }

    tcp_close_connection();
    return 0;
}
