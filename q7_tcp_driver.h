#ifndef Q7_TCP_DRIVER_H
#define Q7_TCP_DRIVER_H

#include <stdint.h>

#define HEADER_SIZE      11
#define MAX_PACKET_SIZE  256
#define MAX_PAYLOAD_SIZE (MAX_PACKET_SIZE - HEADER_SIZE)  /* 245 bytes */

#define IDX_TYPE         0
#define IDX_SEQ_MSB      1
#define IDX_SEQ_LSB      2
#define IDX_FRAG_ID_MSB  3
#define IDX_FRAG_ID_LSB  4
#define IDX_FRAG_TOT_MSB 5
#define IDX_FRAG_TOT_LSB 6
#define IDX_LEN_MSB      7
#define IDX_LEN_LSB      8
#define IDX_CRC_MSB      9
#define IDX_CRC_LSB      10
#define IDX_DATA_START   11

typedef enum {
    PID_CMD_CONTROL  = 0x10,
    PID_PING         = 0x11,
    PID_TELEMETRY_HK = 0x50,
    PID_SCI_IMG      = 0x60,
    PID_SCI_TXT      = 0x61,
    PID_ACK          = 0xAA,
    PID_ERROR        = 0xEE
} PacketType;

typedef struct {
    PacketType type;
    uint16_t   seq_id;
    uint16_t   frag_id;
    uint16_t   frag_total;
    uint16_t   payload_length;
    uint8_t    data[MAX_PAYLOAD_SIZE];
} RawPacket;

int  tcp_init_connection(const char *ip, uint16_t port);
void tcp_close_connection(void);
void send_data(PacketType type, uint16_t payload_length, uint8_t *data);
int  receive_packet(RawPacket *pkt);

#endif
