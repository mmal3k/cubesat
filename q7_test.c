#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h>
#include <dirent.h>

#include "q7_udp_driver.h"

#define BEACON_INTERVAL_S 10 /* send PING + TELEMETRY_HK every N seconds */

static int send_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "[IMG] Cannot open file: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size <= 0)
    {
        fprintf(stderr, "[IMG] File is empty\n");
        fclose(f);
        return -1;
    }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf)
    {
        fprintf(stderr, "[IMG] malloc failed\n");
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size)
    {
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

static void send_file_list(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir)
    {
        fprintf(stderr, "[FILES] Cannot open directory: %s\n", dir_path);
        return;
    }

    /* Dynamically grown buffer — doubles when full, no fixed-size overflow */
    size_t cap = 4096;
    size_t used = 0;
    char *buf = (char *)malloc(cap);
    if (!buf)
    {
        fprintf(stderr, "[FILES] malloc failed\n");
        closedir(dir);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        size_t name_len = strlen(entry->d_name);
        size_t needed = used + name_len + 2; /* '\n' + '\0' */

        if (needed > cap)
        {
            size_t new_cap = cap * 2;
            while (new_cap < needed)
                new_cap *= 2;
            char *tmp = (char *)realloc(buf, new_cap);
            if (!tmp)
            {
                fprintf(stderr, "[FILES] realloc failed - list truncated\n");
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

static int send_file(const char *path)
{
    return send_image(path);
}

static int send_file_from_fragment(const char *path, uint16_t start_frag,
                                   uint16_t resume_seq)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[IMG] Cannot open file for resume: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size <= 0) {
        fclose(f);
        return -1;
    }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    printf("[TX] Resuming image: %s from frag %u (seq=%u)\n",
           path, start_frag, resume_seq);
    send_data_resume(PID_SCI_IMG, (uint32_t)file_size, buf, start_frag, resume_seq);
    free(buf);
    return 0;
}

static void send_latest_image(void)
{
    const char *latest_img = "latest_image.bin";
    printf("[TX] Sending latest image\n");
    send_image(latest_img);
}

static const char *packet_type_name(PacketType type)
{
    switch (type)
    {
    case PID_CMD_CONTROL:
        return "cmd_control";
    case PID_PING:
        return "ping";
    case PID_TELEMETRY_HK:
        return "telemetry_hk";
    case PID_SCI_IMG:
        return "sci_img";
    case PID_SCI_TXT:
        return "sci_txt";
    case PID_ACK:
        return "ack";
    case PID_ERROR:
        return "error";
    case PID_LIST_FILES:
        return "list_files";
    case PID_LATEST_IMG:
        return "latest_img";
    case PID_GET_FILE:
        return "get_file";
    default:
        return "unknown";
    }
}
static void send_beacon(void)
{
    uint8_t payload[] = "HELLO FROM Q7";
    printf("[TX] Beacon: PING + TELEMETRY_HK\n");
    send_data(PID_PING, 0, NULL);
    send_data(PID_TELEMETRY_HK, (uint32_t)(sizeof(payload) - 1), payload);
}

int main(int argc, char *argv[])
{
    uint16_t    local_port = 5001;  /* Q7 listens here for commands */
    uint16_t    gs_port    = 5000;  /* GS listens here for beacons  */
    const char *img_path   = NULL;

    if (argc >= 2) local_port = (uint16_t)atoi(argv[1]);
    if (argc >= 3) gs_port    = (uint16_t)atoi(argv[2]);
    if (argc >= 4) img_path   = argv[3];

    if (udp_init(local_port, gs_port) != 0)
    {
        fprintf(stderr, "Failed to init UDP (local=%u gs=%u)\n",
                local_port, gs_port);
        return 1;
    }

    /* Initial beacon */
    send_beacon();
    if (img_path)
    {
        send_image(img_path);
    }

    printf("[q7] beacon interval: %d s  |  ctrl-c to stop\n", BEACON_INTERVAL_S);

    int fd = udp_get_fd();

    while (1)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv = {.tv_sec = BEACON_INTERVAL_S, .tv_usec = 0};
        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);

        if (ret < 0)
        {
            perror("[q7] select");
            break;
        }

        if (ret == 0)
        {
            /* timeout — send next beacon */
            send_beacon();
            continue;
        }

        /* packet available — receive and handle */
        RawPacket pkt;
        if (receive_packet(&pkt) < 0)
        {
            fprintf(stderr, "[RX] Failed to receive packet, exiting.\n");
            break;
        }

        printf("[RX] [%s] seq=%u frag=%u/%u len=%u\n",
               packet_type_name(pkt.type),
               pkt.seq_id, pkt.frag_id + 1, pkt.frag_total,
               pkt.payload_length);

        if (pkt.payload_length > 0)
        {
            printf("     Payload: %.*s\n", (int)pkt.payload_length, pkt.data);
        }

        /* Dispatch incoming commands */
        if (pkt.type == PID_CMD_CONTROL)
        {
            printf("[TX] Sending ACK for seq=%u\n", pkt.seq_id);
            send_data(PID_ACK, 0, NULL);
        }
        else if (pkt.type == PID_LIST_FILES)
        {
            printf("[CMD] Ground station requested file list\n");
            send_file_list(".");
        }
        else if (pkt.type == PID_LATEST_IMG)
        {
            printf("[CMD] Ground station requested latest image\n");
            send_latest_image();
        }
        else if (pkt.type == PID_GET_FILE)
        {
            if (pkt.payload_length == 0 ||
                pkt.payload_length >= MAX_PAYLOAD_SIZE)
            {
                fprintf(stderr, "[CMD] Invalid filename length: %u\n",
                        pkt.payload_length);
            }
            else
            {
                char path[MAX_PAYLOAD_SIZE + 1];
                memcpy(path, pkt.data, pkt.payload_length);
                path[pkt.payload_length] = '\0';

                if (strstr(path, "..") != NULL)
                {
                    fprintf(stderr, "[CMD] Path traversal rejected: %s\n", path);
                }
                else
                {
                    printf("[CMD] Ground station requested file: %s\n", path);
                    send_file(path);
                }
            }
        }
        else if (pkt.type == PID_RESUME_REQ)
        {
            /* Payload: [resume_seq_MSB][resume_seq_LSB]
             *          [start_frag_MSB][start_frag_LSB]
             *          [filename bytes...] */
            if (pkt.payload_length < 5)
            {
                fprintf(stderr, "[CMD] RESUME_REQ too short\n");
            }
            else
            {
                uint16_t resume_seq = ((uint16_t)pkt.data[0] << 8) | pkt.data[1];
                uint16_t start_frag = ((uint16_t)pkt.data[2] << 8) | pkt.data[3];
                uint16_t fname_len  = pkt.payload_length - 4;

                char path[MAX_PAYLOAD_SIZE + 1];
                memcpy(path, pkt.data + 4, fname_len);
                path[fname_len] = '\0';

                if (strstr(path, "..") != NULL)
                {
                    fprintf(stderr, "[CMD] Resume path traversal rejected: %s\n", path);
                }
                else
                {
                    printf("[CMD] Resume requested: %s from frag %u (seq=%u)\n",
                           path, start_frag, resume_seq);
                    send_file_from_fragment(path, start_frag, resume_seq);
                }
            }
        }
    }

    udp_close();
    return 0;
}