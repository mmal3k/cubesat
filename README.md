# CubeSat Communication System

TCP-based command and telemetry communication between a ground station PC and the Xiphos Q7 processor onboard a CubeSat (UPEC, launch 2027).

---

## Overview

The satellite connects to the ground station over TCP/Ethernet during a ~10-minute pass window. The Q7 acts as a **TCP client** and connects to the ground station PC (TCP server). Once connected, both sides can send and receive packets using the custom protocol described below.

```
Ground Station PC  ←────── TCP over Ethernet ──────→  Xiphos Q7 (ARM Linux)
  ground_station.py                                   q7_tcp_driver.c
  (server, port 5000)                                 (client)
```

---

## Hardware

| Component | Details |
|-----------|---------|
| Processor | Xiphos Q7 Rev B — Xilinx Zynq 7020 (ARM Cortex-A9HF + FPGA) |
| OS | Yocto 2.5, Linux 4.14 LTS |
| RAM | 2 GiB LPDDR4 ECC |
| Storage | 2× 128 MB QSPI NOR Flash |
| Network | Gigabit Ethernet via Q7-J4 connector |
| Toolchain | GCC 7.3.0 via Yocto SDK |

---

## Packet Protocol

Every transmission uses a fixed **11-byte header** followed by up to 245 bytes of payload.

```
Byte(s)   Field            Description
──────────────────────────────────────────────────────────
[0]       Packet Type      See PacketType enum below
[1–2]     Sequence ID      MSB first, auto-incremented per send_data() call
[3–4]     Fragment ID      MSB first, 0-indexed
[5–6]     Total Fragments  MSB first
[7–8]     Payload Length   MSB first, bytes in this fragment
[9–10]    CRC16            16-bit additive checksum (CRC bytes zeroed before compute)
[11+]     Payload          Up to 245 bytes
```

### Packet Types

| Constant | Value | Purpose |
|----------|-------|---------|
| `PID_CMD_CONTROL` | `0x10` | Command from ground station to Q7 |
| `PID_PING` | `0x11` | Heartbeat / keepalive |
| `PID_TELEMETRY_HK` | `0x50` | Housekeeping telemetry |
| `PID_SCI_IMG` | `0x60` | Science image data |
| `PID_SCI_TXT` | `0x61` | Science text data |
| `PID_ACK` | `0xAA` | Acknowledgement |
| `PID_ERROR` | `0xEE` | Error response |

### Fragmentation

Large payloads are automatically split into 245-byte fragments by `send_data()`. The receiver reassembles them using `seq_id` and `frag_id`. The ground station's Python implementation handles fragment reassembly in `receive_loop()`.

### Checksum

A simple 16-bit additive checksum over the entire packet (header + payload), with the CRC bytes set to zero during computation.

---

## File Structure

```
cubesat/
├── q7_tcp_driver.h      Protocol constants, packet struct, public API
├── q7_tcp_driver.c      TCP client driver (Q7 side) — connect, send, receive
├── q7_tcp_test.c        Q7-side test program — sends ping, telemetry, optional image
├── ground_station.py    PC-side server — receives packets, sends commands interactively
├── build.sh             Cross-compilation script (sources Xiphos SDK, runs make)
└── Makefile             Build rules (currently targets INA260 sensor test, see note below)
```

---

## Building

### For the Q7 (ARM cross-compilation)

The Xiphos SDK must be installed at `/opt/xiphos/sdk/ark/`.

```bash
./build.sh
```

This sources the SDK environment (which sets `CC`, `CFLAGS`, `LDFLAGS` to the ARM toolchain) and runs `make`.

> **Note:** The Makefile currently references `ina260_tests.c` and `monitor_driver.c` from an earlier sensor test phase. To build the TCP driver instead, update `SRCS` and `TARGET` in the Makefile accordingly.

### For local testing (host machine)

```bash
gcc q7_tcp_driver.c q7_tcp_test.c -o q7_tcp_test
```

---

## Running

### 1. Start the ground station (PC)

```bash
python3 ground_station.py [port]
# Default: listens on 0.0.0.0:5000
```

The ground station prints received packets and accepts interactive commands:

```
ping          Send a PING to the Q7
ack           Send an ACK
cmd <text>    Send a CMD_CONTROL with a text payload
quit          Disconnect
```

Received `SCI_IMG` packets are automatically saved as `image_<seq>.bin` and displayed if [Pillow](https://pillow.readthedocs.io/) is installed.

### 2. Start the Q7 client

```bash
./q7_tcp_test [ip] [port] [image_file]
# Defaults: 127.0.0.1, 5000, no image
```

On connect, the Q7:
1. Sends a `PING`
2. Sends a `TELEMETRY_HK` packet with payload `"HELLO FROM Q7"`
3. Sends the specified image file as `PID_SCI_IMG` (if provided)
4. Enters a receive loop — prints any incoming packets and ACKs any `CMD_CONTROL` packets

### Deploying to Q7 via SCP

```bash
scp q7_tcp_test root@<Q7_IP>:/tmp/
ssh root@<Q7_IP> /tmp/q7_tcp_test <PC_IP> 5000
```

---

## C API (`q7_tcp_driver.h`)

```c
// Connect to the ground station TCP server
int tcp_init_connection(const char *ip, uint16_t port);

// Close the connection
void tcp_close_connection(void);

// Send a packet (auto-fragments payloads > 245 bytes)
void send_data(PacketType type, uint32_t payload_length, uint8_t *data);

// Receive one packet fragment (blocks until data arrives)
// Returns 0 on success, -1 on error or checksum failure
int receive_packet(RawPacket *pkt);
```

`RawPacket` struct:

```c
typedef struct {
    PacketType type;
    uint16_t   seq_id;
    uint16_t   frag_id;
    uint16_t   frag_total;
    uint16_t   payload_length;
    uint8_t    data[245];
} RawPacket;
```

---

## Q7 Access

```bash
# Serial console (115200 8N1) via PIM micro-USB → FTDI adapter
# SSH
ssh root@<Q7_IP>          # no default password

# Enable USB if needed
q7hw usb set on

# File transfer
scp <file> root@<Q7_IP>:/tmp/
```

---

## Dependencies

| Component | Requirement |
|-----------|-------------|
| Q7 build | Xiphos SDK at `/opt/xiphos/sdk/ark/`, Vivado 2018.2 for FPGA |
| Host build | `gcc` |
| Ground station | Python 3, optional: `pip install Pillow` (for image display) |
