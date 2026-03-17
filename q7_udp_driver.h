#ifndef Q7_UDP_DRIVER_H
#define Q7_UDP_DRIVER_H

#include <stdint.h>

/*
 * Packet header layout (13 bytes):
 *
 *  [0]     Packet Type      (1 byte)
 *  [1-2]   Sequence ID      (2 bytes, MSB first)
 *  [3-5]   Fragment ID      (3 bytes, MSB first — supports up to 16 M frags)
 *  [6-8]   Total Fragments  (3 bytes, MSB first)
 *  [9-10]  Payload Length   (2 bytes, MSB first)
 *  [11-12] CRC16            (2 bytes, MSB first)
 *  [13+]   Payload          (up to 243 bytes)
 */
#define HEADER_SIZE      13
#define MAX_PACKET_SIZE  256
#define MAX_PAYLOAD_SIZE (MAX_PACKET_SIZE - HEADER_SIZE) /* 243 bytes */

#define ACK_TIMEOUT_MS   5000   /* ms to wait for an ACK before retrying  */
#define MAX_RETRIES      3      /* max retransmissions before giving up    */

#define IDX_TYPE          0
#define IDX_SEQ_MSB       1
#define IDX_SEQ_LSB       2
#define IDX_FRAG_ID_MSB   3   /* bits 23-16 */
#define IDX_FRAG_ID_MID   4   /* bits 15-8  */
#define IDX_FRAG_ID_LSB   5   /* bits  7-0  */
#define IDX_FRAG_TOT_MSB  6   /* bits 23-16 */
#define IDX_FRAG_TOT_MID  7   /* bits 15-8  */
#define IDX_FRAG_TOT_LSB  8   /* bits  7-0  */
#define IDX_LEN_MSB       9
#define IDX_LEN_LSB      10
#define IDX_CRC_MSB      11
#define IDX_CRC_LSB      12
#define IDX_DATA_START   13

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
    PID_FILE_HDR     = 0x62,  /* filename announcement — sent before PID_SCI_IMG */
    PID_ACK          = 0xAA,
    PID_NACK         = 0xBB,
    PID_ERROR        = 0xEE
} PacketType;

typedef struct
{
    PacketType type;
    uint16_t   seq_id;
    uint32_t   frag_id;      /* 24-bit field stored in 32-bit int */
    uint32_t   frag_total;   /* 24-bit field stored in 32-bit int */
    uint16_t   payload_length;
    uint8_t    data[MAX_PAYLOAD_SIZE];
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
                      uint32_t start_frag, uint16_t resume_seq);
int  receive_packet(RawPacket *pkt);

#endif
