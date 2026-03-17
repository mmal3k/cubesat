#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <dirent.h>

#include "q7_udp_driver.h"

#define BEACON_INTERVAL_S 30 /* send PING + TELEMETRY_HK every N seconds */
#define IMAGE_DIR "./data/images"  /* default image directory on Q7 */

/* ------------------------------------------------------------------ */
/*  File helpers                                                        */
/* ------------------------------------------------------------------ */

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

    /* Announce the filename so GS can store it for cross-pass resume */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    send_data(PID_FILE_HDR, (uint32_t)strlen(base), (uint8_t *)base);

    printf("[TX] Sending image: %s (%ld bytes)\n", path, file_size);
    send_data(PID_SCI_IMG, (uint32_t)file_size, buf);
    free(buf);
    return 0;
}

/* Returns 1 if str ends with suffix, 0 otherwise */
static int ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix)
        return 0;
    size_t len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (len < suffix_len)
        return 0;
    return strcmp(str + len - suffix_len, suffix) == 0;
}

/* Returns 1 if filename has a recognised image extension */
static int is_image_file(const char *filename)
{
    return ends_with(filename, ".jpg") ||
           ends_with(filename, ".jpeg") ||
           ends_with(filename, ".png") ||
           ends_with(filename, ".bin");
}

/*
 * send_file_list – enumerate dir_path, list image files, transmit as PID_SCI_TXT.
 *
 * Always sends a response so the ground station never waits in silence:
 *   - opendir failure  → sends an error string
 *   - no images found  → sends "(no image files found)"
 *   - success          → sends newline-separated filenames
 */
static void send_file_list(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir)
    {
        fprintf(stderr, "[FILES] Cannot open directory: %s\n", dir_path);
        /* FIX 1: send error response so GS always gets something */
        const char *err = "(error: could not open directory)";
        send_data(PID_SCI_TXT, (uint32_t)strlen(err), (uint8_t *)err);
        return;
    }

    /* Dynamically grown buffer — doubles when full */
    size_t cap = 4096;
    size_t used = 0;
    char *buf = (char *)malloc(cap);
    if (!buf)
    {
        fprintf(stderr, "[FILES] malloc failed\n");
        closedir(dir);
        const char *err = "(error: out of memory)";
        send_data(PID_SCI_TXT, (uint32_t)strlen(err), (uint8_t *)err);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (!is_image_file(entry->d_name))
            continue;

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

    /* FIX 2: send placeholder when directory has no matching files
     * so the GS never receives a 0-byte payload (which it silently drops) */
    if (used == 0)
    {
        free(buf);
        const char *empty = "(no image files found)";
        printf("[TX] Sending file list (none found in %s)\n", dir_path);
        send_data(PID_SCI_TXT, (uint32_t)strlen(empty), (uint8_t *)empty);
        return;
    }

    printf("[TX] Sending file list (%zu bytes, dir=%s)\n", used, dir_path);
    send_data(PID_SCI_TXT, (uint32_t)used, (uint8_t *)buf);
    free(buf);
}

/*
 * send_latest_image – find the most recently modified image in dir_path
 * and transmit it.
 *
 * FIX 3: stat() is now called with the full path (dir_path + "/" + name)
 *        so it works regardless of the process's working directory.
 * FIX 1: sends an error response if nothing is found so GS never waits
 *        in silence.
 */
static int send_file_from_fragment(const char *path, uint32_t start_frag,
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

static void send_latest_image(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir)
    {
        fprintf(stderr, "[CMD] Cannot open directory: %s\n", dir_path);
        const char *err = "(error: could not open directory)";
        send_data(PID_SCI_TXT, (uint32_t)strlen(err), (uint8_t *)err);
        return;
    }

    struct dirent *entry;
    struct stat file_stat;
    char newest_file[256] = {0};
    time_t newest_time = 0;
    int found = 0; /* FIX: explicit flag, not newest_time==0 */

    while ((entry = readdir(dir)) != NULL)
    {
        if (!is_image_file(entry->d_name))
            continue;

        /* FIX 3: build full path so stat() works from any cwd */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 dir_path, entry->d_name);

        if (stat(full_path, &file_stat) == 0)
        {
            if (!found || file_stat.st_mtime > newest_time)
            {
                newest_time = file_stat.st_mtime;
                strncpy(newest_file, entry->d_name, sizeof(newest_file) - 1);
                found = 1;
            }
        }
    }
    closedir(dir);

    if (!found)
    {
        printf("[TX] No image files found in %s\n", dir_path);
        const char *err = "(no image files found)";
        send_data(PID_SCI_TXT, (uint32_t)strlen(err), (uint8_t *)err);
        return;
    }

    /* Build the full path for send_image() as well */
    char send_path[512];
    snprintf(send_path, sizeof(send_path), "%s/%s", dir_path, newest_file);
    printf("[TX] Found latest image: %s\n", newest_file);
    send_image(send_path);
}

/* ------------------------------------------------------------------ */
/*  Logging helper                                                      */
/* ------------------------------------------------------------------ */

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
    case PID_RESUME_REQ:
        return "resume_req";
    case PID_NACK:
        return "nack";
    default:
        return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  Beacon                                                              */
/* ------------------------------------------------------------------ */

static void send_beacon(void)
{
    uint8_t payload[] = "HELLO FROM Q7";
    printf("[TX] Beacon: PING + TELEMETRY_HK\n");
    send_data(PID_PING, 0, NULL);
    send_data(PID_TELEMETRY_HK, (uint32_t)(sizeof(payload) - 1), payload);
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    uint16_t local_port = 5001;
    uint16_t gs_port = 5000;
    const char *img_path = NULL;

    if (argc >= 2) local_port = (uint16_t)atoi(argv[1]);
    if (argc >= 3) gs_port    = (uint16_t)atoi(argv[2]);
    if (argc >= 4) img_path   = argv[3];

    if (udp_init(local_port, gs_port) != 0)
    {
        fprintf(stderr, "Failed to init UDP (local=%u gs=%u)\n",
                local_port, gs_port);
        return 1;
    }

    send_beacon();
    if (img_path)
        send_image(img_path);

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
            send_beacon();
            continue;
        }

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

        if (pkt.payload_length > 0 && pkt.type != PID_SCI_IMG)
            printf("     Payload: %.*s\n", (int)pkt.payload_length, pkt.data);

        /* Dispatch incoming commands */
        if (pkt.type == PID_CMD_CONTROL)
        {
            printf("[TX] Sending ACK for seq=%u\n", pkt.seq_id);
            send_data(PID_ACK, 0, NULL);
        }
        else if (pkt.type == PID_LIST_FILES)
        {
            printf("[CMD] Ground station requested file list\n");
            send_file_list(IMAGE_DIR);
        }
        else if (pkt.type == PID_LATEST_IMG)
        {
            printf("[CMD] Ground station requested latest image\n");
            send_latest_image(IMAGE_DIR);
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
                char name[MAX_PAYLOAD_SIZE + 1];
                memcpy(name, pkt.data, pkt.payload_length);
                name[pkt.payload_length] = '\0';

                if (strstr(name, "..") != NULL)
                {
                    fprintf(stderr, "[CMD] Path traversal rejected: %s\n", name);
                }
                else
                {
                    char path[sizeof(IMAGE_DIR) + MAX_PAYLOAD_SIZE + 2];
                    if (name[0] == '/')
                        snprintf(path, sizeof(path), "%s", name);
                    else
                        snprintf(path, sizeof(path), "%s/%s", IMAGE_DIR, name);
                    printf("[CMD] Ground station requested file: %s\n", path);
                    send_image(path);
                }
            }
        }
        else if (pkt.type == PID_RESUME_REQ)
        {
            /* Payload: [resume_seq_MSB][resume_seq_LSB]
             *          [start_frag_B2][start_frag_B1][start_frag_B0]
             *          [filename bytes...] */
            if (pkt.payload_length < 6)
            {
                fprintf(stderr, "[CMD] RESUME_REQ too short\n");
            }
            else
            {
                uint16_t resume_seq = ((uint16_t)pkt.data[0] << 8) | pkt.data[1];
                uint32_t start_frag = ((uint32_t)pkt.data[2] << 16)
                                    | ((uint32_t)pkt.data[3] <<  8)
                                    |  (uint32_t)pkt.data[4];
                uint16_t fname_len  = pkt.payload_length - 5;

                char name[MAX_PAYLOAD_SIZE + 1];
                memcpy(name, pkt.data + 5, fname_len);
                name[fname_len] = '\0';

                if (strstr(name, "..") != NULL)
                {
                    fprintf(stderr, "[CMD] Resume path traversal rejected: %s\n", name);
                }
                else
                {
                    char path[sizeof(IMAGE_DIR) + MAX_PAYLOAD_SIZE + 2];
                    if (name[0] == '/')
                        snprintf(path, sizeof(path), "%s", name);
                    else
                        snprintf(path, sizeof(path), "%s/%s", IMAGE_DIR, name);
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