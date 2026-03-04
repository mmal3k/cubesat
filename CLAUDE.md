# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a CubeSat communication system implementing TCP-based command and telemetry exchange between a ground station and a Q7 processor (ARM Cortex-A9HF running Linux via the Xiphos SDK).

## Build

Cross-compilation targets the Q7 ARM processor using the Xiphos SDK toolchain.

```bash
./build.sh        # Sources the Q7 SDK env and runs make
make clean        # Remove object files and executables
```

The SDK must be installed at `/opt/xiphos/sdk/ark/` for `build.sh` to work. `CC`, `CFLAGS`, and `LDFLAGS` are injected by the SDK environment script — do not hardcode these in source.

To run the TCP test program directly (on host for local testing):
```bash
gcc q7_tcp_driver.c q7_tcp_test.c -o q7_tcp_test
./q7_tcp_test [ip] [port]       # defaults: 127.0.0.1, 5000
```

## Architecture

### Communication Protocol (`q7_tcp_driver.h` / `q7_tcp_driver.c`)

The core of the project is a custom TCP packet protocol with an 11-byte header:

```
[0]     Packet Type  (PacketType enum)
[1-2]   Sequence ID  (MSB first)
[3-4]   Fragment ID  (MSB first)
[5-6]   Total Fragments (MSB first)
[7-8]   Payload Length (MSB first)
[9-10]  CRC16
[11+]   Payload (up to 245 bytes per fragment)
```

Packet types (`PacketType` enum):
- `PID_CMD_CONTROL (0x10)` — commands
- `PID_PING (0x11)` — heartbeat
- `PID_TELEMETRY_HK (0x50)` — housekeeping telemetry
- `PID_SCI_IMG (0x60)` / `PID_SCI_TXT (0x61)` — science data
- `PID_ACK (0xAA)` / `PID_ERROR (0xEE)` — responses

Large payloads are automatically fragmented into 245-byte chunks by `send_data()`.

### Public API

```c
int  tcp_init_connection(const char *ip, uint16_t port);
void tcp_close_connection(void);
void send_data(PacketType type, uint16_t payload_length, uint8_t *data);
```

### Hardware Context

The Makefile (currently targeting INA260 sensor tests) references `ina260_tests.c` and `monitor_driver.c` — these may be on another branch or pending addition. The TCP driver (`q7_tcp_driver.c`) is the actively developed communication layer.
