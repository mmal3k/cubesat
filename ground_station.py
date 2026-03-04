#!/usr/bin/env python3
"""
Ground Station - TCP Server
Communicates with the Q7 using the custom packet protocol.
"""

import socket
import threading
import sys

# --- Protocol constants ---
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

_seq_id = 0
_seq_lock = threading.Lock()


# --- Checksum ---

def calculate_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


# --- Packet builder ---

def build_packets(pkt_type: int, payload: bytes = b'') -> list:
    global _seq_id

    frag_total = max(1, (len(payload) + MAX_PAYLOAD_SIZE - 1) // MAX_PAYLOAD_SIZE)
    packets = []

    with _seq_lock:
        seq = _seq_id
        _seq_id = (_seq_id + 1) & 0xFFFF

    for frag_id in range(frag_total):
        offset     = frag_id * MAX_PAYLOAD_SIZE
        chunk      = payload[offset:offset + MAX_PAYLOAD_SIZE]
        chunk_size = len(chunk)

        header = bytearray(HEADER_SIZE)
        header[IDX_TYPE]         = pkt_type
        header[IDX_SEQ_MSB]      = (seq >> 8) & 0xFF
        header[IDX_SEQ_LSB]      = seq & 0xFF
        header[IDX_FRAG_ID_MSB]  = (frag_id >> 8) & 0xFF
        header[IDX_FRAG_ID_LSB]  = frag_id & 0xFF
        header[IDX_FRAG_TOT_MSB] = (frag_total >> 8) & 0xFF
        header[IDX_FRAG_TOT_LSB] = frag_total & 0xFF
        header[IDX_LEN_MSB]      = (chunk_size >> 8) & 0xFF
        header[IDX_LEN_LSB]      = chunk_size & 0xFF
        header[IDX_CRC_MSB]      = 0x00
        header[IDX_CRC_LSB]      = 0x00

        raw = bytes(header) + chunk
        crc = calculate_checksum(raw)
        header[IDX_CRC_MSB] = (crc >> 8) & 0xFF
        header[IDX_CRC_LSB] = crc & 0xFF

        packets.append(bytes(header) + chunk)

    return packets


# --- Packet receiver ---

def recv_exact(sock, n: int) -> bytes:
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Connection closed by Q7")
        buf += chunk
    return buf


def receive_packet(sock) -> dict:
    header = recv_exact(sock, HEADER_SIZE)

    pkt_type    = header[IDX_TYPE]
    seq_id      = (header[IDX_SEQ_MSB]      << 8) | header[IDX_SEQ_LSB]
    frag_id     = (header[IDX_FRAG_ID_MSB]  << 8) | header[IDX_FRAG_ID_LSB]
    frag_total  = (header[IDX_FRAG_TOT_MSB] << 8) | header[IDX_FRAG_TOT_LSB]
    payload_len = (header[IDX_LEN_MSB]      << 8) | header[IDX_LEN_LSB]
    recv_crc    = (header[IDX_CRC_MSB]      << 8) | header[IDX_CRC_LSB]

    payload = recv_exact(sock, payload_len) if payload_len > 0 else b''

    # Verify checksum
    raw = bytearray(header)
    raw[IDX_CRC_MSB] = 0x00
    raw[IDX_CRC_LSB] = 0x00
    computed_crc = calculate_checksum(bytes(raw) + payload)
    crc_ok = (computed_crc == recv_crc)

    if not crc_ok:
        print(f"[WARN] Checksum mismatch: expected 0x{computed_crc:04X}, got 0x{recv_crc:04X}")

    return {
        'type':           pkt_type,
        'type_name':      PACKET_TYPE_NAMES.get(pkt_type, f"UNKNOWN(0x{pkt_type:02X})"),
        'seq_id':         seq_id,
        'frag_id':        frag_id,
        'frag_total':     frag_total,
        'payload_length': payload_len,
        'payload':        payload,
        'crc_ok':         crc_ok,
    }


# --- Receive thread (with fragment reassembly) ---

def receive_loop(sock):
    # reassembly buffer: seq_id -> {'total': N, 'type': T, 'frags': {frag_id: bytes}}
    reassembly = {}

    while True:
        try:
            pkt = receive_packet(sock)
        except (ConnectionError, OSError) as e:
            print(f"\n[GS] Connection lost: {e}")
            break

        seq_id     = pkt['seq_id']
        frag_id    = pkt['frag_id']
        frag_total = pkt['frag_total']

        if seq_id not in reassembly:
            reassembly[seq_id] = {'total': frag_total, 'type': pkt['type'], 'frags': {}}

        reassembly[seq_id]['frags'][frag_id] = pkt['payload']

        if len(reassembly[seq_id]['frags']) == frag_total:
            # All fragments received — reassemble
            full_payload = b''.join(
                reassembly[seq_id]['frags'][i] for i in range(frag_total)
            )
            pkt_type = reassembly[seq_id]['type']
            del reassembly[seq_id]

            type_name  = PACKET_TYPE_NAMES.get(pkt_type, f"UNKNOWN(0x{pkt_type:02X})")
            crc_status = "OK" if pkt['crc_ok'] else "FAIL"

            print(f"\n[RX] [{type_name}] seq={seq_id} len={len(full_payload)} crc={crc_status}")
            if full_payload:
                try:
                    print(f"     {full_payload.decode('utf-8', errors='replace')}")
                except Exception:
                    print(f"     (hex) {full_payload.hex()}")
            print("> ", end='', flush=True)


# --- Send helper ---

def send_packet(sock, pkt_type: int, payload: bytes = b''):
    packets = build_packets(pkt_type, payload)
    for p in packets:
        sock.sendall(p)
    name = PACKET_TYPE_NAMES.get(pkt_type, f"0x{pkt_type:02X}")
    print(f"[TX] [{name}] {len(payload)} bytes")


# --- Main ---

def main():
    host = '0.0.0.0'
    port = 5000

    if len(sys.argv) >= 2:
        port = int(sys.argv[1])

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((host, port))
    server.listen(1)

    print(f"[GS] Ground station listening on port {port}...")
    conn, addr = server.accept()
    print(f"[GS] Q7 connected from {addr[0]}:{addr[1]}")
    print()

    rx_thread = threading.Thread(target=receive_loop, args=(conn,), daemon=True)
    rx_thread.start()

    print("Commands:")
    print("  ping            - send a PING to the Q7")
    print("  ack             - send an ACK")
    print("  cmd <text>      - send a CMD_CONTROL with a text payload")
    print("  quit            - disconnect")
    print()

    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            break

        if not line:
            continue
        elif line == 'ping':
            send_packet(conn, PID_PING)
        elif line == 'ack':
            send_packet(conn, PID_ACK)
        elif line.startswith('cmd '):
            send_packet(conn, PID_CMD_CONTROL, line[4:].encode('utf-8'))
        elif line == 'quit':
            break
        else:
            print("Unknown command. Try: ping | ack | cmd <text> | quit")

    conn.close()
    server.close()
    print("[GS] Disconnected.")


if __name__ == '__main__':
    main()
