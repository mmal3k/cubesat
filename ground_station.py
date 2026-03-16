#!/usr/bin/env python3
"""
Ground Station (UDP)

Flow:
  1. Binds to gs_port, waits for a Q7 beacon (broadcast)
  2. Learns the Q7 address from the first recvfrom()
  3. Sends commands unicast to Q7, receives and reassembles responses

Usage:
  python3 ground_station.py [gs_port] [q7_port]
  Default: gs_port=5000, q7_port=5001
"""

import socket
import threading
import sys
import io
import os
from collections import deque

try:
    from PIL import Image
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False

# ── Protocol constants ──────────────────────────────────────────
HEADER_SIZE      = 11
MAX_PACKET_SIZE  = 256
MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - HEADER_SIZE  # 245 bytes

IDX_TYPE         = 0
IDX_SEQ_MSB      = 1
IDX_SEQ_LSB      = 2
IDX_FRAG_ID_MSB  = 3
IDX_FRAG_ID_LSB  = 4
IDX_FRAG_TOT_MSB = 5
IDX_FRAG_TOT_LSB = 6
IDX_LEN_MSB      = 7
IDX_LEN_LSB      = 8
IDX_CRC_MSB      = 9
IDX_CRC_LSB      = 10

PID_CMD_CONTROL  = 0x10
PID_PING         = 0x11
PID_TELEMETRY_HK = 0x50
PID_SCI_IMG      = 0x60
PID_SCI_TXT      = 0x61
PID_ACK          = 0xAA
PID_ERROR        = 0xEE

PACKET_TYPE_NAMES = {
    PID_CMD_CONTROL:  "CMD_CONTROL",
    PID_PING:         "PING",
    PID_TELEMETRY_HK: "TELEMETRY_HK",
    PID_SCI_IMG:      "SCI_IMG",
    PID_SCI_TXT:      "SCI_TXT",
    PID_ACK:          "ACK",
    PID_ERROR:        "ERROR",
}

# ── Sequence counter ────────────────────────────────────────────
_seq_id   = 0
_seq_lock = threading.Lock()

# ── Q7 address — learned from the first beacon ──────────────────
_q7_addr       = None
_q7_addr_lock  = threading.Lock()
_q7_addr_ready = threading.Event()

# ── Duplicate detection — keep last 256 seen seq_ids ────────────
_seen_seqs = deque(maxlen=256)


# ── Checksum ────────────────────────────────────────────────────
def calculate_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


# ── ACK builder ─────────────────────────────────────────────────
def build_ack(seq_id: int) -> bytes:
    header = bytearray(HEADER_SIZE)
    header[IDX_TYPE]         = PID_ACK
    header[IDX_SEQ_MSB]      = (seq_id >> 8) & 0xFF
    header[IDX_SEQ_LSB]      =  seq_id       & 0xFF
    header[IDX_FRAG_ID_MSB]  = 0
    header[IDX_FRAG_ID_LSB]  = 0
    header[IDX_FRAG_TOT_MSB] = 0
    header[IDX_FRAG_TOT_LSB] = 1
    header[IDX_LEN_MSB]      = 0
    header[IDX_LEN_LSB]      = 0
    header[IDX_CRC_MSB]      = 0
    header[IDX_CRC_LSB]      = 0
    crc = calculate_checksum(bytes(header))
    header[IDX_CRC_MSB] = (crc >> 8) & 0xFF
    header[IDX_CRC_LSB] =  crc       & 0xFF
    return bytes(header)


# ── Packet builder ──────────────────────────────────────────────
def build_packets(pkt_type: int, payload: bytes = b'') -> list:
    global _seq_id
    frag_total = max(1, (len(payload) + MAX_PAYLOAD_SIZE - 1) // MAX_PAYLOAD_SIZE)
    packets    = []

    with _seq_lock:
        seq     = _seq_id
        _seq_id = (_seq_id + 1) & 0xFFFF

    for frag_id in range(frag_total):
        offset     = frag_id * MAX_PAYLOAD_SIZE
        chunk      = payload[offset:offset + MAX_PAYLOAD_SIZE]
        chunk_size = len(chunk)

        header = bytearray(HEADER_SIZE)
        header[IDX_TYPE]         = pkt_type
        header[IDX_SEQ_MSB]      = (seq        >> 8) & 0xFF
        header[IDX_SEQ_LSB]      =  seq               & 0xFF
        header[IDX_FRAG_ID_MSB]  = (frag_id    >> 8) & 0xFF
        header[IDX_FRAG_ID_LSB]  =  frag_id           & 0xFF
        header[IDX_FRAG_TOT_MSB] = (frag_total >> 8) & 0xFF
        header[IDX_FRAG_TOT_LSB] =  frag_total        & 0xFF
        header[IDX_LEN_MSB]      = (chunk_size >> 8) & 0xFF
        header[IDX_LEN_LSB]      =  chunk_size        & 0xFF
        header[IDX_CRC_MSB]      = 0x00
        header[IDX_CRC_LSB]      = 0x00

        raw = bytes(header) + chunk
        crc = calculate_checksum(raw)
        header[IDX_CRC_MSB] = (crc >> 8) & 0xFF
        header[IDX_CRC_LSB] =  crc        & 0xFF

        packets.append(bytes(header) + chunk)

    return packets


# ── Packet parser ────────────────────────────────────────────────
def parse_packet(data: bytes):
    if len(data) < HEADER_SIZE:
        return None

    pkt_type    = data[IDX_TYPE]
    seq_id      = (data[IDX_SEQ_MSB]      << 8) | data[IDX_SEQ_LSB]
    frag_id     = (data[IDX_FRAG_ID_MSB]  << 8) | data[IDX_FRAG_ID_LSB]
    frag_total  = (data[IDX_FRAG_TOT_MSB] << 8) | data[IDX_FRAG_TOT_LSB]
    payload_len = (data[IDX_LEN_MSB]      << 8) | data[IDX_LEN_LSB]
    recv_crc    = (data[IDX_CRC_MSB]      << 8) | data[IDX_CRC_LSB]

    if HEADER_SIZE + payload_len > len(data):
        print("[WARN] Datagram shorter than declared payload length")
        return None

    payload = data[HEADER_SIZE:HEADER_SIZE + payload_len]

    raw = bytearray(data[:HEADER_SIZE + payload_len])
    raw[IDX_CRC_MSB] = 0x00
    raw[IDX_CRC_LSB] = 0x00
    computed_crc = calculate_checksum(bytes(raw))
    crc_ok = (computed_crc == recv_crc)

    if not crc_ok:
        print(f"[WARN] CRC mismatch seq={seq_id}: "
              f"expected 0x{computed_crc:04X}, got 0x{recv_crc:04X}")

    return {
        'type':       pkt_type,
        'type_name':  PACKET_TYPE_NAMES.get(pkt_type, f"UNKNOWN(0x{pkt_type:02X})"),
        'seq_id':     seq_id,
        'frag_id':    frag_id,
        'frag_total': frag_total,
        'payload':    payload,
        'crc_ok':     crc_ok,
    }


# ── Image handler ────────────────────────────────────────────────
def _handle_image(data: bytes, seq_id: int):
    filename = f"image_{seq_id}.bin"
    with open(filename, 'wb') as f:
        f.write(data)
    print(f"     Saved to {os.path.abspath(filename)}")

    if not _PIL_AVAILABLE:
        print("     (install Pillow to display: pip install Pillow)")
        return
    try:
        img = Image.open(io.BytesIO(data))
        print(f"     Displaying: {img.width}x{img.height} {img.format}")
        img.show()
    except Exception as e:
        print(f"     Could not display image: {e}")


# ── Receive thread ───────────────────────────────────────────────
def receive_loop(sock, q7_port: int):
    global _q7_addr
    reassembly = {}  # seq_id -> {'total': N, 'type': T, 'frags': {frag_id: bytes}}

    # Packet types that should never be ACKed
    NO_ACK_TYPES = {PID_PING, PID_ACK, PID_ERROR}

    while True:
        try:
            data, addr = sock.recvfrom(MAX_PACKET_SIZE)
        except OSError:
            break

        # Learn Q7 address from the first beacon
        with _q7_addr_lock:
            if _q7_addr is None:
                _q7_addr = (addr[0], q7_port)
                _q7_addr_ready.set()
                print(f"\n[GS] Beacon from {addr[0]} — pass window active")
                print(f"[GS] Sending commands to {addr[0]}:{q7_port}")
                print("> ", end='', flush=True)

        pkt = parse_packet(data)
        if pkt is None:
            continue

        seq_id     = pkt['seq_id']
        frag_id    = pkt['frag_id']
        frag_total = pkt['frag_total']
        pkt_type   = pkt['type']

        # Accumulate fragments
        if seq_id not in reassembly:
            reassembly[seq_id] = {'total': frag_total, 'type': pkt_type, 'frags': {}}
        reassembly[seq_id]['frags'][frag_id] = pkt['payload']

        # Wait until all fragments of this message have arrived
        if len(reassembly[seq_id]['frags']) < frag_total:
            continue

        # ── Full message reassembled ──────────────────────────────
        full_payload = b''.join(
            reassembly[seq_id]['frags'][i] for i in range(frag_total)
        )
        del reassembly[seq_id]

        with _q7_addr_lock:
            q7_addr = _q7_addr

        # Send ACK (mirrors seq_id so Q7 knows which message was received)
        if pkt_type not in NO_ACK_TYPES and q7_addr is not None:
            sock.sendto(build_ack(seq_id), q7_addr)
            print(f"\n[GS] ACK sent for seq={seq_id}")

        # Duplicate detection — discard if we already processed this seq_id
        if seq_id in _seen_seqs:
            print(f"[GS] Duplicate seq={seq_id} — discarded")
            print("> ", end='', flush=True)
            continue

        _seen_seqs.append(seq_id)

        # ── Process new message ───────────────────────────────────
        type_name = PACKET_TYPE_NAMES.get(pkt_type, f"UNKNOWN(0x{pkt_type:02X})")
        crc_str   = "OK" if pkt['crc_ok'] else "FAIL"

        print(f"\n[RX] [{type_name}]  seq={seq_id}  "
              f"len={len(full_payload)}  crc={crc_str}")

        if pkt_type == PID_SCI_IMG:
            _handle_image(full_payload, seq_id)
        elif full_payload:
            try:
                print(f"     {full_payload.decode('utf-8', errors='replace')}")
            except Exception:
                print(f"     (hex) {full_payload.hex()}")

        print("> ", end='', flush=True)


# ── Send helper ──────────────────────────────────────────────────
def send_packet(sock, pkt_type: int, payload: bytes = b''):
    if not _q7_addr_ready.is_set():
        print("[GS] No beacon received yet — waiting for Q7")
        return

    with _q7_addr_lock:
        addr = _q7_addr

    for p in build_packets(pkt_type, payload):
        sock.sendto(p, addr)

    name = PACKET_TYPE_NAMES.get(pkt_type, f"0x{pkt_type:02X}")
    print(f"[TX] [{name}]  {len(payload)} bytes  → {addr[0]}:{addr[1]}")


# ── Main ─────────────────────────────────────────────────────────
def main():
    gs_port = 5000   # GS listens here  (Q7 broadcasts beacons to this port)
    q7_port = 5001   # Q7 listens here  (GS sends commands to this port)

    if len(sys.argv) >= 2: gs_port = int(sys.argv[1])
    if len(sys.argv) >= 3: q7_port = int(sys.argv[2])

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', gs_port))

    print("╔══════════════════════════════════════════╗")
    print("║               Ground Station             ║")
    print("╚══════════════════════════════════════════╝")
    print(f"Listening for beacons on port {gs_port}")
    print(f"Commands will go to Q7 port  {q7_port}")
    print()
    print("Commands:  ping | cmd <text> | quit")
    print("Waiting for Q7 beacon...")
    print()

    rx = threading.Thread(target=receive_loop, args=(sock, q7_port), daemon=True)
    rx.start()

    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            break

        if not line:
            continue
        elif line == 'ping':
            send_packet(sock, PID_PING)
        elif line == 'ack':
            send_packet(sock, PID_ACK)
        elif line.startswith('cmd '):
            send_packet(sock, PID_CMD_CONTROL, line[4:].encode())
        elif line == 'quit':
            break
        else:
            print("Unknown command.  Try:  ping | cmd <text> | quit")

    sock.close()
    print("[GS] Closed.")


if __name__ == '__main__':
    main()
