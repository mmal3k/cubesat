#ifndef Q7_UDP_DRIVER_H
#define Q7_UDP_DRIVER_H

#include <stdint.h>

#define HEADER_SIZE 11
#define MAX_PACKET_SIZE 256
#define MAX_PAYLOAD_SIZE (MAX_PACKET_SIZE - HEADER_SIZE) /* 245 bytes */

#define ACK_TIMEOUT_MS   5000   /* ms to wait for an ACK before retrying  */
#define MAX_RETRIES      3      /* max retransmissions before giving up    */

#define IDX_TYPE         0
#define IDX_SEQ_MSB      1
#define IDX_SEQ_LSB      2
#define IDX_FRAG_ID_MSB  3
#define IDX_FRAG_ID_LSB  4
#define IDX_FRAG_TOT_MSB 5
#define IDX_FRAG_TOT_LSB 6
#define IDX_LEN_MSB 7
#define IDX_LEN_LSB 8
#define IDX_CRC_MSB 9
#define IDX_CRC_LSB 10
#define IDX_DATA_START 11

typedef enum
{
    PID_CMD_CONTROL  = 0x10,
    PID_PING         = 0x11,
    PID_LIST_FILES   = 0x20,
    PID_LATEST_IMG   = 0x21,
    PID_GET_FILE     = 0x22,
    PID_RESUME_REQ   = 0x30,
    PID_TELEMETRY_HK = 0x50,
    PID_SCI_IMG      = 0x60,
    PID_SCI_TXT      = 0x61,
    PID_ACK          = 0xAA,
    PID_NACK         = 0xBB,
    PID_ERROR        = 0xEE
} PacketType;

typedef struct
{
    PacketType type;
    uint16_t seq_id;
    uint16_t frag_id;
    uint16_t frag_total;
    uint16_t payload_length;
    uint8_t data[MAX_PAYLOAD_SIZE];
} RawPacket;

/*
 * Initialise the UDP socket.
 *
 * local_port : port the Q7 binds to (to receive commands from the GS)
 * gs_port    : port the GS listens on (beacons are broadcast to this port)
 *
 * Beacons are sent to 255.255.255.255:gs_port until the GS address is
 * learned from the first incoming packet, after which replies go unicast.
 */
int  udp_init(uint16_t local_port, uint16_t gs_port);
void udp_close(void);
int  udp_get_fd(void);
void send_data(PacketType type, uint32_t payload_length, uint8_t *data);
void send_data_resume(PacketType type, uint32_t payload_length, uint8_t *data,
                      uint16_t start_frag, uint16_t resume_seq);
int  receive_packet(RawPacket *pkt);

#endif