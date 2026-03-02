#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "q7_tcp_driver.h"

int main(int argc, char *argv[]) {
    const char *ip = "127.0.0.1";
    uint16_t port = 5000;

    if (argc >= 2) {
        ip = argv[1];
    }
    if (argc >= 3) {
        int p = atoi(argv[2]);
        if (p > 0 && p <= 65535) {
            port = (uint16_t)p;
        }
    }

    if (tcp_init_connection(ip, port) != 0) {
        fprintf(stderr, "Failed to connect to %s:%u\n", ip, (unsigned)port);
        return 1;
    }

    uint8_t payload[] = "HELLO FROM Q7";
    send_data(PID_TELEMETRY_HK, (uint16_t)(sizeof(payload) - 1), payload);

    tcp_close_connection();
    return 0;
}

