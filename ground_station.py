#!/usr/bin/env python3
"""
Ground Station - TCP Server
Communicates with the Q7 using the custom packet protocol.

* Tracks the last fully-reassembled sequence ID across sessions.
* On every new Q7 connection, immediately sends PID_RETRANSMIT_REQ
  so the Q7 replays anything lost during the outage.
* Loops back to accept() automatically after a disconnect.
"""

import socket
import threading
import sys
import io
import os

try:
    from PIL import Image
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False

# ------------------------------------------------------------------ #
#  Protocol constants                                                  #
# ------------------------------------------------------------------ #

HEADER_SIZE      = 11
MAX_PACKET_SIZE  = 256
MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - HEADER_SIZE   # 245 bytes

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

# Downlink: Q7 -> Ground
PID_CMD_CONTROL    = 0x10
PID_PING           = 0x11
PID_TELEMETRY_HK   = 0x50
PID_SCI_IMG        = 0x60
PID_SCI_TXT        = 0x61
PID_ACK            = 0xAA
PID_ERROR          = 0xEE
# Uplink commands: Ground -> Q7
PID_LIST_FILES     = 0x20
PID_LATEST_IMG     = 0x21
PID_GET_FILE       = 0x22
# Reliability
PID_RETRANSMIT_REQ = 0xBB

PACKET_TYPE_NAMES = {
    PID_CMD_CONTROL:    "CMD_CONTROL",
    PID_PING:           "PING",
    PID_TELEMETRY_HK:   "TELEMETRY_HK",
    PID_SCI_IMG:        "SCI_IMG",
    PID_SCI_TXT:        "SCI_TXT",
    PID_ACK:            "ACK",
    PID_ERROR:          "ERROR",
    PID_LIST_FILES:     "LIST_FILES",
    PID_LATEST_IMG:     "LATEST_IMG",
    PID_GET_FILE:       "GET_FILE",
    PID_RETRANSMIT_REQ: "RETRANSMIT_REQ",
}

# ------------------------------------------------------------------ #
#  Shared state                                                        #
# ------------------------------------------------------------------ #

_gs_lock = threading.Lock()

# Last fully-reassembled Q7 sequence ID (None = no session yet).
_last_complete_seq: int | None = None

_seq_id = 0   # outgoing sequence counter


# ------------------------------------------------------------------ #
#  Checksum                                                            #
# ------------------------------------------------------------------ #

def calculate_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


# ------------------------------------------------------------------ #
#  Packet builder                                                      #
# ------------------------------------------------------------------ #

def build_packets(pkt_type: int, payload: bytes = b'') -> list[bytes]:
    global _seq_id

    frag_total = max(1, (len(payload) + MAX_PAYLOAD_SIZE - 1) // MAX_PAYLOAD_SIZE)
    packets    = []

    with _gs_lock:
        seq     = _seq_id
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


# ------------------------------------------------------------------ #
#  Packet receiver                                                     #
# ------------------------------------------------------------------ #

def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Connection closed by Q7")
        buf += chunk
    return buf


def receive_packet(sock: socket.socket) -> dict:
    header = recv_exact(sock, HEADER_SIZE)

    pkt_type    = header[IDX_TYPE]
    seq_id      = (header[IDX_SEQ_MSB]      << 8) | header[IDX_SEQ_LSB]
    frag_id     = (header[IDX_FRAG_ID_MSB]  << 8) | header[IDX_FRAG_ID_LSB]
    frag_total  = (header[IDX_FRAG_TOT_MSB] << 8) | header[IDX_FRAG_TOT_LSB]
    payload_len = (header[IDX_LEN_MSB]      << 8) | header[IDX_LEN_LSB]
    recv_crc    = (header[IDX_CRC_MSB]      << 8) | header[IDX_CRC_LSB]

    payload = recv_exact(sock, payload_len) if payload_len > 0 else b''

    raw = bytearray(header)
    raw[IDX_CRC_MSB] = 0x00
    raw[IDX_CRC_LSB] = 0x00
    computed_crc = calculate_checksum(bytes(raw) + payload)
    crc_ok       = (computed_crc == recv_crc)

    if not crc_ok:
        print(f"[WARN] CRC mismatch seq={seq_id}: "
              f"expected 0x{computed_crc:04X}, got 0x{recv_crc:04X}")

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


# ------------------------------------------------------------------ #
#  Image handler                                                       #
# ------------------------------------------------------------------ #

def _handle_image(data: bytes, seq_id: int):
    filename = f"image_{seq_id}.bin"
    with open(filename, 'wb') as f:
        f.write(data)
    print(f"     Image saved to {os.path.abspath(filename)}")

    if not _PIL_AVAILABLE:
        print("     (install Pillow to display: pip install Pillow)")
        return
    try:
        img = Image.open(io.BytesIO(data))
        print(f"     Displaying image: {img.width}x{img.height} {img.format}")
        img.show()
    except Exception as exc:
        print(f"     Could not display image: {exc}")


# ------------------------------------------------------------------ #
#  Receive thread                                                      #
# ------------------------------------------------------------------ #

def receive_loop(sock: socket.socket, stop_event: threading.Event):
    global _last_complete_seq

    reassembly: dict = {}

    while not stop_event.is_set():
        try:
            pkt = receive_packet(sock)
        except (ConnectionError, OSError) as exc:
            if not stop_event.is_set():
                print(f"\n[GS] Connection lost: {exc}")
            stop_event.set()
            break

        seq_id     = pkt['seq_id']
        frag_id    = pkt['frag_id']
        frag_total = pkt['frag_total']

        if seq_id not in reassembly:
            reassembly[seq_id] = {
                'total': frag_total,
                'type':  pkt['type'],
                'frags': {},
            }

        reassembly[seq_id]['frags'][frag_id] = pkt['payload']

        if len(reassembly[seq_id]['frags']) == frag_total:
            full_payload = b''.join(
                reassembly[seq_id]['frags'][i] for i in range(frag_total)
            )
            pkt_type = reassembly[seq_id]['type']
            del reassembly[seq_id]

            type_name  = PACKET_TYPE_NAMES.get(pkt_type, f"UNKNOWN(0x{pkt_type:02X})")
            crc_status = "OK" if pkt['crc_ok'] else "FAIL"

            print(f"\n[RX] [{type_name}] seq={seq_id} "
                  f"len={len(full_payload)} crc={crc_status}")

            # Track last good sequence for retransmit requests
            with _gs_lock:
                if _last_complete_seq is None:
                    _last_complete_seq = seq_id
                else:
                    expected = (_last_complete_seq + 1) & 0xFFFF
                    if seq_id != expected:
                        print(f"[GS] GAP: expected seq {expected}, got {seq_id}")
                    _last_complete_seq = seq_id

            if pkt_type == PID_SCI_IMG:
                _handle_image(full_payload, seq_id)
            elif full_payload:
                try:
                    print(f"     {full_payload.decode('utf-8', errors='replace')}")
                except Exception:
                    print(f"     (hex) {full_payload.hex()}")

            print("> ", end='', flush=True)


# ------------------------------------------------------------------ #
#  Send helpers                                                        #
# ------------------------------------------------------------------ #

def send_packet(sock: socket.socket, pkt_type: int, payload: bytes = b''):
    packets = build_packets(pkt_type, payload)
    for p in packets:
        sock.sendall(p)
    name = PACKET_TYPE_NAMES.get(pkt_type, f"0x{pkt_type:02X}")
    print(f"[TX] [{name}] {len(payload)} bytes")


def send_retransmit_request(sock: socket.socket):
    """Ask the Q7 to replay everything after our last known good sequence."""
    with _gs_lock:
        last = _last_complete_seq

    if last is None:
        payload = bytes([0xFF, 0xFF])
        print("[GS] No prior session - requesting full retransmit")
    else:
        payload = bytes([(last >> 8) & 0xFF, last & 0xFF])
        print(f"[GS] Requesting retransmit of packets after seq={last}")

    send_packet(sock, PID_RETRANSMIT_REQ, payload)


# ------------------------------------------------------------------ #
#  CMD sub-menu                                                        #
# ------------------------------------------------------------------ #

def cmd_menu(conn: socket.socket):
    while True:
        print()
        print("CMD MENU:")
        print("  1    - Show list of files on Q7")
        print("  2    - Request latest image")
        print("  3    - Request file by path")
        print("  back - return to main menu")
        print()

        choice = input("cmd> ").strip()

        if choice == "1":
            send_packet(conn, PID_LIST_FILES)

        elif choice == "2":
            send_packet(conn, PID_LATEST_IMG)

        elif choice == "3":
            path = input("Enter file path: ").strip()
            if path:
                send_packet(conn, PID_GET_FILE, path.encode())
            else:
                print("No path entered.")

        elif choice == "back":
            break

        else:
            print("Invalid choice. Enter 1, 2, 3, or back.")


# ------------------------------------------------------------------ #
#  Input thread                                                        #
# ------------------------------------------------------------------ #

def input_loop(sock_ref: list, stop_event: threading.Event):
    """
    Runs in its own thread so it never blocks the receive loop.
    sock_ref[0] is updated by main() on every new connection.
    """
    print("Commands: ping | ack | cmd | quit")
    print()

    while not stop_event.is_set():
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            stop_event.set()
            break

        if not line:
            continue

        sock = sock_ref[0]
        if sock is None:
            print("[GS] No active connection.")
            continue

        try:
            if line == 'ping':
                send_packet(sock, PID_PING)
            elif line == 'ack':
                send_packet(sock, PID_ACK)
            elif line == 'cmd':
                cmd_menu(sock)
            elif line == 'quit':
                stop_event.set()
                break
            else:
                print("Unknown command. Try: ping | ack | cmd | quit")
        except OSError as exc:
            print(f"[GS] Send failed: {exc}")


# ------------------------------------------------------------------ #
#  Main - reconnect loop                                               #
# ------------------------------------------------------------------ #

def main():
    host = '0.0.0.0'
    port = 5000

    if len(sys.argv) >= 2:
        port = int(sys.argv[1])

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((host, port))
    server.listen(1)
    server.settimeout(1.0)

    print(f"[GS] Ground station listening on port {port}  (Ctrl-C to quit)")
    print()

    global_stop = threading.Event()
    sock_ref    = [None]

    input_thread = threading.Thread(
        target=input_loop,
        args=(sock_ref, global_stop),
        daemon=True,
    )
    input_thread.start()

    session = 0

    while not global_stop.is_set():
        print("[GS] Waiting for Q7 connection...")
        conn = None

        while not global_stop.is_set():
            try:
                conn, addr = server.accept()
                break
            except socket.timeout:
                continue
            except OSError:
                break

        if conn is None or global_stop.is_set():
            break

        session += 1
        print(f"\n[GS] Q7 connected from {addr[0]}:{addr[1]}  "
              f"(session #{session})\n")

        sock_ref[0] = conn

        try:
            send_retransmit_request(conn)
        except OSError as exc:
            print(f"[GS] Could not send retransmit request: {exc}")
            conn.close()
            sock_ref[0] = None
            continue

        link_lost = threading.Event()
        rx_thread = threading.Thread(
            target=receive_loop,
            args=(conn, link_lost),
            daemon=True,
        )
        rx_thread.start()

        while not link_lost.is_set() and not global_stop.is_set():
            rx_thread.join(timeout=0.5)

        print(f"\n[GS] Session #{session} ended.")
        conn.close()
        sock_ref[0] = None

        if global_stop.is_set():
            break

        print("[GS] Waiting for next connection...\n")

    server.close()
    print("[GS] Ground station shut down.")


if __name__ == '__main__':
    main()
