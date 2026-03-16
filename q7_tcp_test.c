#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>

#include "q7_tcp_driver.h"

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

/* FIX 4 (cont): moved here from q7_tcp_driver.c so send_image() is visible */
static void send_file_list(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir)
    {
        fprintf(stderr, "[FILES] Cannot open directory\n");
        return;
    }

    char buffer[4096] = {0};
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        strcat(buffer, entry->d_name);
        strcat(buffer, "\n");
    }

    closedir(dir);

    printf("[TX] Sending file list\n");
    send_data(PID_SCI_TXT, strlen(buffer), (uint8_t *)buffer);
}

static int send_file(const char *path)
{
    return send_image(path);
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
        return "CMD_CONTROL";
    case PID_PING:
        return "PING";
    case PID_TELEMETRY_HK:
        return "TELEMETRY_HK";
    case PID_SCI_IMG:
        return "SCI_IMG";
    case PID_SCI_TXT:
        return "SCI_TXT";
    case PID_ACK:
        return "ACK";
    case PID_ERROR:
        return "ERROR";
    case PID_LIST_FILES:
        return "LIST_FILES";
    case PID_LATEST_IMG:
        return "LATEST_IMG";
    case PID_GET_FILE:
        return "GET_FILE";
    default:
        return "UNKNOWN";
    }
}

int main(int argc, char *argv[])
{
    const char *ip = "127.0.0.1";
    uint16_t port = 5000;
    const char *img_path = NULL;

    if (argc >= 2)
        ip = argv[1];
    if (argc >= 3)
        port = (uint16_t)atoi(argv[2]);
    if (argc >= 4)
        img_path = argv[3];

    if (tcp_init_connection(ip, port) != 0)
    {
        fprintf(stderr, "Failed to connect to %s:%u\n", ip, (unsigned)port);
        return 1;
    }

    printf("[TX] Sending PING\n");
    send_data(PID_PING, 0, NULL);

    uint8_t payload[] = "HELLO FROM Q7";
    printf("[TX] Sending TELEMETRY_HK: \"%s\"\n", payload);
    send_data(PID_TELEMETRY_HK, (uint16_t)(sizeof(payload) - 1), payload);

    if (img_path)
    {
        send_image(img_path);
    }

    printf("[RX] Waiting for packets from ground station...\n");
    while (1)
    {
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

        if (pkt.type == PID_LIST_FILES)
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
            printf("[CMD] Ground station requested file: %.*s\n",
                   pkt.payload_length, pkt.data);

            char path[256];
            memcpy(path, pkt.data, pkt.payload_length);
            path[pkt.payload_length] = '\0';

            send_file(path);
        }
    }

    tcp_close_connection();
    return 0;
}