#!/usr/bin/env python3
"""
Ground Station (UDP)

Flow:
  1. Binds to gs_port, waits for a Q7 beacon (broadcast)
  2. Learns the Q7 address from the first recvfrom()
  3. Sends commands unicast to Q7, receives and reassembles responses

Resume flow (across passes):
  - Incomplete messages are saved to partial/ on timeout
  - On next beacon, GS sends RESUME_REQ for each saved partial file
  - Q7 resumes sending from the last received fragment

Usage:
  python3 ground_station.py [gs_port] [q7_port]
  Default: gs_port=5000, q7_port=5001
"""

import socket
import threading
import sys
import io
import os
import json
import time
import glob
import binascii
from collections import deque

try:
    from PIL import Image
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False

# ── Protocol constants ──────────────────────────────────────────
HEADER_SIZE      = 13
MAX_PACKET_SIZE  = 256
MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - HEADER_SIZE  # 243 bytes

IDX_TYPE          = 0
IDX_SEQ_MSB       = 1
IDX_SEQ_LSB       = 2
IDX_FRAG_ID_MSB   = 3   # bits 23-16
IDX_FRAG_ID_MID   = 4   # bits 15-8
IDX_FRAG_ID_LSB   = 5   # bits  7-0
IDX_FRAG_TOT_MSB  = 6   # bits 23-16
IDX_FRAG_TOT_MID  = 7   # bits 15-8
IDX_FRAG_TOT_LSB  = 8   # bits  7-0
IDX_LEN_MSB       = 9
IDX_LEN_LSB       = 10
IDX_CRC_MSB       = 11
IDX_CRC_LSB       = 12

PID_CMD_CONTROL  = 0x10
PID_PING         = 0x11
PID_LIST_FILES   = 0x20
PID_LATEST_IMG   = 0x21
PID_GET_FILE     = 0x22
PID_RESUME_REQ   = 0x30
PID_TELEMETRY_HK = 0x50
PID_SCI_IMG      = 0x60
PID_SCI_TXT      = 0x61
PID_FILE_HDR     = 0x62
PID_ACK          = 0xAA
PID_NACK         = 0xBB
PID_ERROR        = 0xEE

PACKET_TYPE_NAMES = {
    PID_CMD_CONTROL:  "CMD_CONTROL",
    PID_PING:         "PING",
    PID_LIST_FILES:   "LIST_FILES",
    PID_LATEST_IMG:   "LATEST_IMG",
    PID_GET_FILE:     "GET_FILE",
    PID_RESUME_REQ:   "RESUME_REQ",
    PID_TELEMETRY_HK: "TELEMETRY_HK",
    PID_SCI_IMG:      "SCI_IMG",
    PID_SCI_TXT:      "SCI_TXT",
    PID_FILE_HDR:     "FILE_HDR",
    PID_ACK:          "ACK",
    PID_NACK:         "NACK",
    PID_ERROR:        "ERROR",
}

# ── Tuning ──────────────────────────────────────────────────────
FRAGMENT_TIMEOUT_S = 3    # seconds after last frag before sending NACK
PARTIAL_DIR        = 'partial'

# ── Sequence counter ────────────────────────────────────────────
_seq_id   = 0
_seq_lock = threading.Lock()

# ── Q7 address — learned from the first beacon ──────────────────
_q7_addr       = None
_q7_addr_lock  = threading.Lock()
_q7_addr_ready = threading.Event()

# ── Shared reassembly dict (receive_loop + timeout_checker) ─────
# seq_id → {total, type, frags, last_frag_time, filename}
_reassembly      = {}
_reassembly_lock = threading.Lock()

# ── Duplicate detection — keep last 256 seen seq_ids ────────────
_seen_seqs = deque(maxlen=256)

# ── Last requested filename (for resume tracking) ────────────────
_last_requested_file      = None
_last_requested_file_lock = threading.Lock()

# ── Pending file announced by Q7 but not yet fully received ──────
# Set when FILE_HDR arrives; cleared when the image completes.
# Used to re-request the file if Q7 restarts before sending any fragments.
_pending_file      = None
_pending_file_lock = threading.Lock()


# ── Checksum ────────────────────────────────────────────────────
def calculate_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


# ── Packet builders ─────────────────────────────────────────────
def _build_control_packet(pkt_type: int, seq_id: int, payload: bytes) -> bytes:
    """Build a single-fragment packet with an explicit seq_id (for ACK/NACK)."""
    chunk_size = len(payload)
    header = bytearray(HEADER_SIZE)
    header[IDX_TYPE]          = pkt_type
    header[IDX_SEQ_MSB]       = (seq_id     >> 8) & 0xFF
    header[IDX_SEQ_LSB]       =  seq_id            & 0xFF
    header[IDX_FRAG_ID_MSB]   = 0
    header[IDX_FRAG_ID_MID]   = 0
    header[IDX_FRAG_ID_LSB]   = 0
    header[IDX_FRAG_TOT_MSB]  = 0
    header[IDX_FRAG_TOT_MID]  = 0
    header[IDX_FRAG_TOT_LSB]  = 1
    header[IDX_LEN_MSB]       = (chunk_size >> 8) & 0xFF
    header[IDX_LEN_LSB]       =  chunk_size        & 0xFF
    header[IDX_CRC_MSB]       = 0
    header[IDX_CRC_LSB]       = 0
    raw = bytes(header) + payload
    crc = calculate_checksum(raw)
    header[IDX_CRC_MSB] = (crc >> 8) & 0xFF
    header[IDX_CRC_LSB] =  crc       & 0xFF
    return bytes(header) + payload


def build_ack(seq_id: int) -> bytes:
    return _build_control_packet(PID_ACK, seq_id, b'')


def build_nack(seq_id: int, missing_frags: list) -> bytes:
    """Build a NACK with the list of missing fragment IDs (3 bytes each)."""
    max_frags = (MAX_PAYLOAD_SIZE - 2) // 3   # 80
    frags = missing_frags[:max_frags]
    payload = len(frags).to_bytes(2, 'big')
    for fid in frags:
        payload += fid.to_bytes(3, 'big')
    return _build_control_packet(PID_NACK, seq_id, payload)


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
        header[IDX_TYPE]          = pkt_type
        header[IDX_SEQ_MSB]       = (seq        >> 8) & 0xFF
        header[IDX_SEQ_LSB]       =  seq               & 0xFF
        header[IDX_FRAG_ID_MSB]   = (frag_id    >> 16) & 0xFF
        header[IDX_FRAG_ID_MID]   = (frag_id    >>  8) & 0xFF
        header[IDX_FRAG_ID_LSB]   =  frag_id            & 0xFF
        header[IDX_FRAG_TOT_MSB]  = (frag_total >> 16) & 0xFF
        header[IDX_FRAG_TOT_MID]  = (frag_total >>  8) & 0xFF
        header[IDX_FRAG_TOT_LSB]  =  frag_total         & 0xFF
        header[IDX_LEN_MSB]       = (chunk_size >>  8) & 0xFF
        header[IDX_LEN_LSB]       =  chunk_size         & 0xFF
        header[IDX_CRC_MSB]       = 0x00
        header[IDX_CRC_LSB]       = 0x00

        raw = bytes(header) + chunk
        crc = calculate_checksum(raw)
        header[IDX_CRC_MSB] = (crc >> 8) & 0xFF
        header[IDX_CRC_LSB] =  crc       & 0xFF

        packets.append(bytes(header) + chunk)

    return packets


# ── Packet parser ────────────────────────────────────────────────
def parse_packet(data: bytes):
    if len(data) < HEADER_SIZE:
        return None

    pkt_type    = data[IDX_TYPE]
    seq_id      = (data[IDX_SEQ_MSB]     << 8) | data[IDX_SEQ_LSB]
    frag_id     = (data[IDX_FRAG_ID_MSB]  << 16) | (data[IDX_FRAG_ID_MID]  << 8) | data[IDX_FRAG_ID_LSB]
    frag_total  = (data[IDX_FRAG_TOT_MSB] << 16) | (data[IDX_FRAG_TOT_MID] << 8) | data[IDX_FRAG_TOT_LSB]
    payload_len = (data[IDX_LEN_MSB]     << 8) | data[IDX_LEN_LSB]
    recv_crc    = (data[IDX_CRC_MSB]     << 8) | data[IDX_CRC_LSB]

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
RECEIVED_DIR = "received"

def _handle_image(data: bytes, seq_id: int):
    os.makedirs(RECEIVED_DIR, exist_ok=True)
    filename = os.path.join(RECEIVED_DIR, f"image_{seq_id}.bin")
    with open(filename, 'wb') as f:
        f.write(data)
    print(f"     Saved to {os.path.abspath(filename)}")

    if not _PIL_AVAILABLE:
        print("     (install Pillow to display: pip install Pillow)")
        return
    try:
        # Image.MAX_IMAGE_PIXELS = None  # disable decompression bomb check
        img = Image.open(io.BytesIO(data))
        print(f"     Displaying: {img.width}x{img.height} {img.format}")
        img.show()
    except Exception as e:
        print(f"     Could not display image: {e}")


# ── Partial transfer persistence ────────────────────────────────
def save_partial(seq_id: int, msg: dict):
    """Save an incomplete message to disk for resume on next pass."""
    os.makedirs(PARTIAL_DIR, exist_ok=True)
    meta = {
        'seq_id':        seq_id,
        'pkt_type':      msg['type'],
        'frag_total':    msg['total'],
        'received_frags': sorted(msg['frags'].keys()),
        'filename':      msg.get('filename'),
    }
    meta_path = os.path.join(PARTIAL_DIR, f'seq_{seq_id:05d}.json')
    with open(meta_path, 'w') as f:
        json.dump(meta, f)
    for frag_id, data in msg['frags'].items():
        frag_path = os.path.join(PARTIAL_DIR,
                                 f'seq_{seq_id:05d}_frag_{frag_id:05d}.bin')
        with open(frag_path, 'wb') as f:
            f.write(data)
    print(f"[GS] Partial saved: seq={seq_id} "
          f"({len(msg['frags'])}/{msg['total']} frags, file={msg.get('filename')})")


def _cleanup_partial(seq_id: int):
    """Delete saved partial files for a completed transfer."""
    for path in glob.glob(os.path.join(PARTIAL_DIR, f'seq_{seq_id:05d}*.bin')):
        os.remove(path)
    meta_path = os.path.join(PARTIAL_DIR, f'seq_{seq_id:05d}.json')
    if os.path.exists(meta_path):
        os.remove(meta_path)


def load_and_resume_partials(sock):
    """
    Called after beacon is received.
    For each saved partial transfer, pre-populate reassembly and send RESUME_REQ.
    """
    if not os.path.exists(PARTIAL_DIR):
        return

    for meta_path in sorted(glob.glob(os.path.join(PARTIAL_DIR, 'seq_*.json'))):
        try:
            with open(meta_path) as f:
                meta = json.load(f)
        except Exception:
            continue

        seq_id     = meta['seq_id']
        frag_total = meta['frag_total']
        received   = set(meta['received_frags'])
        filename   = meta.get('filename')

        missing = [i for i in range(frag_total) if i not in received]
        if not missing:
            _cleanup_partial(seq_id)
            continue

        if not filename:
            print(f"[GS] Cannot resume seq={seq_id}: no filename stored")
            continue

        last_received = max(received)
        start_frag    = last_received + 1

        print(f"[GS] Resuming partial transfer: seq={seq_id}  "
              f"have {len(received)}/{frag_total} frags  "
              f"file={filename}  resuming from frag {start_frag}")

        # Load saved fragments back into the reassembly dict
        frags = {}
        for frag_id in received:
            frag_path = os.path.join(PARTIAL_DIR,
                                     f'seq_{seq_id:05d}_frag_{frag_id:05d}.bin')
            if os.path.exists(frag_path):
                with open(frag_path, 'rb') as f:
                    frags[frag_id] = f.read()

        with _reassembly_lock:
            if seq_id in _reassembly:
                # A live transfer with this seq_id is already in progress — don't
                # overwrite it with stale partial data.
                print(f"[GS] Skipping partial load for seq={seq_id}: already in progress")
                continue
            _reassembly[seq_id] = {
                'total':          frag_total,
                'type':           meta['pkt_type'],
                'frags':          frags,
                'last_frag_time': time.time(),
                'filename':       filename,
                'from_partial':   True,
            }

        # Remove from _seen_seqs so the reassembled message isn't discarded
        try:
            _seen_seqs.remove(seq_id)
        except ValueError:
            pass

        # Ask Q7 to resume
        # RESUME_REQ payload: [seq_MSB][seq_LSB][start_frag_B2][start_frag_B1][start_frag_B0][filename]
        resume_payload = (seq_id    .to_bytes(2, 'big') +
                          start_frag.to_bytes(3, 'big') +
                          filename.encode())
        send_packet(sock, PID_RESUME_REQ, resume_payload)


# ── Fragment timeout checker (background thread) ─────────────────
def timeout_checker(sock):
    """
    Runs every second. If a message has not received all its fragments
    within FRAGMENT_TIMEOUT_S, send a NACK listing the missing ones
    and save the partial data to disk.
    """
    while True:
        time.sleep(1)
        now = time.time()

        with _reassembly_lock:
            stale = [
                (seq_id, msg) for seq_id, msg in _reassembly.items()
                if now - msg['last_frag_time'] > FRAGMENT_TIMEOUT_S
            ]

        for seq_id, msg in stale:
            missing = [i for i in range(msg['total']) if i not in msg['frags']]
            if not missing:
                continue  # all frags arrived, reassembly logic will clean up

            with _q7_addr_lock:
                q7_addr = _q7_addr

            if q7_addr is not None:
                nack_pkt = build_nack(seq_id, missing)
                sock.sendto(nack_pkt, q7_addr)
                print(f"\n[GS] NACK seq={seq_id}  missing {len(missing)} frags: "
                      f"{missing[:10]}{'...' if len(missing) > 10 else ''}")
                print("> ", end='', flush=True)

            # Reset timer so we don't spam NACKs every second
            with _reassembly_lock:
                if seq_id in _reassembly:
                    _reassembly[seq_id]['last_frag_time'] = now

            # Save to disk so we can resume next pass
            save_partial(seq_id, msg)


# ── Receive thread ───────────────────────────────────────────────
def receive_loop(sock, q7_port: int):
    global _q7_addr, _pending_file

    # Packet types that should never be ACKed
    NO_ACK_TYPES = {PID_PING, PID_ACK, PID_NACK, PID_ERROR, PID_RESUME_REQ}

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
                # Check for incomplete transfers from previous pass
                threading.Thread(target=load_and_resume_partials,
                                 args=(sock,), daemon=True).start()

        pkt = parse_packet(data)
        if pkt is None:
            continue
        if not pkt['crc_ok']:
            # Fragment arrived corrupted — discard it; NACK will request a retransmit
            continue

        seq_id     = pkt['seq_id']
        frag_id    = pkt['frag_id']
        frag_total = pkt['frag_total']
        pkt_type   = pkt['type']

        # Accumulate fragments into shared reassembly dict
        with _reassembly_lock:
            if seq_id not in _reassembly:
                # Track filename for file transfers so we can resume them
                filename = None
                if pkt_type in (PID_SCI_IMG, PID_SCI_TXT):
                    with _last_requested_file_lock:
                        filename = _last_requested_file
                _reassembly[seq_id] = {
                    'total':          frag_total,
                    'type':           pkt_type,
                    'frags':          {},
                    'last_frag_time': time.time(),
                    'filename':       filename,
                }
            elif _reassembly[seq_id]['total'] != frag_total:
                # Stale partial from a different transfer — discard and restart
                old_total = _reassembly[seq_id]['total']
                _cleanup_partial(seq_id)
                filename = None
                if pkt_type in (PID_SCI_IMG, PID_SCI_TXT):
                    with _last_requested_file_lock:
                        filename = _last_requested_file
                print(f"\n[GS] Stale partial for seq={seq_id} discarded "
                      f"(expected {old_total} frags, got {frag_total}) — restarting")
                print("> ", end='', flush=True)
                _reassembly[seq_id] = {
                    'total':          frag_total,
                    'type':           pkt_type,
                    'frags':          {},
                    'last_frag_time': time.time(),
                    'filename':       filename,
                }
            _reassembly[seq_id]['frags'][frag_id]    = pkt['payload']
            _reassembly[seq_id]['last_frag_time']    = time.time()
            frags_received = len(_reassembly[seq_id]['frags'])
            entry_total    = _reassembly[seq_id]['total']

        # Wait until all fragments of this message have arrived
        if frags_received < entry_total:
            continue

        # ── Full message reassembled ──────────────────────────────
        with _reassembly_lock:
            if seq_id not in _reassembly:
                continue  # already processed by another iteration
            msg          = _reassembly.pop(seq_id)
            filename_ref = msg.get('filename')

        full_payload = b''.join(
            msg['frags'][i] for i in range(frag_total)
        )

        with _q7_addr_lock:
            q7_addr = _q7_addr

        # Send ACK (mirrors seq_id so Q7 knows which message was received)
        if pkt_type not in NO_ACK_TYPES and q7_addr is not None:
            sock.sendto(build_ack(seq_id), q7_addr)
            print(f"\n[GS] ACK sent for seq={seq_id}")

        # New pass: Q7 restarted — seq_id wrapped back to an already-seen value
        if pkt_type == PID_PING and seq_id in _seen_seqs:
            _seen_seqs.clear()
            print("\n[GS] Q7 restarted — new pass, cleared sequence history")
            print("> ", end='', flush=True)

            # Update Q7 address in case it changed
            with _q7_addr_lock:
                _q7_addr = (addr[0], q7_port)

            # Save any in-flight transfers to disk and clear reassembly so
            # load_and_resume_partials can reload and resume them cleanly
            with _reassembly_lock:
                inflight = dict(_reassembly)
                _reassembly.clear()
            for sid, msg in inflight.items():
                if msg.get('filename'):
                    missing = [i for i in range(msg['total'])
                               if i not in msg['frags']]
                    if missing:
                        save_partial(sid, msg)

            threading.Thread(target=load_and_resume_partials,
                             args=(sock,), daemon=True).start()

        # Duplicate detection — discard if we already processed this seq_id
        if seq_id in _seen_seqs:
            print(f"[GS] Duplicate seq={seq_id} — discarded")
            print("> ", end='', flush=True)
            continue

        _seen_seqs.append(seq_id)

        # Clean up any saved partial files for this transfer
        _cleanup_partial(seq_id)

        # ── Verify message-level CRC32 (last 4 bytes appended by Q7) ─
        msg_crc_ok = True
        if len(full_payload) >= 4:
            recv_crc32     = int.from_bytes(full_payload[-4:], 'big')
            computed_crc32 = binascii.crc32(full_payload[:-4]) & 0xFFFFFFFF
            msg_crc_ok     = (recv_crc32 == computed_crc32)
            full_payload   = full_payload[:-4]  # strip CRC bytes

        # # ── Silently drop periodic beacons — they would interrupt user input ──
        # if pkt_type in (PID_PING, PID_TELEMETRY_HK):
        #     continue

        # ── Process new message ───────────────────────────────────
        type_name    = PACKET_TYPE_NAMES.get(pkt_type, f"UNKNOWN(0x{pkt_type:02X})")
        frag_crc_str = "OK" if pkt['crc_ok'] else "FAIL"
        msg_crc_str  = "OK" if msg_crc_ok    else "FAIL"

        print(f"\n[RX] [{type_name}]  seq={seq_id}  "
              f"len={len(full_payload)}  frag_crc={frag_crc_str}  msg_crc={msg_crc_str}")

        if not msg_crc_ok:
            if msg.get('from_partial'):
                print("     [!] Message CRC failed — stale partial data; "
                      "partial deleted, please request the file again")
            else:
                print("     [!] Message CRC failed — data corrupted, discarding")
            print("> ", end='', flush=True)
            continue

        if pkt_type == PID_FILE_HDR:
            # Q7 announces the filename before sending the image — store it
            announced = full_payload.decode('utf-8', errors='replace').strip()
            with _last_requested_file_lock:
                _last_requested_file = announced
            with _pending_file_lock:
                _pending_file = announced
            print(f"     File: {announced}")
        elif pkt_type == PID_SCI_IMG:
            with _pending_file_lock:
                _pending_file = None  # transfer complete — no longer pending
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


# ── Command menu ─────────────────────────────────────────────────
def cmd_menu(sock):
    while True:
        print()
        print("CMD MENU:")
        print("1 - Show list of files on Q7")
        print("2 - Request latest image")
        print("3 - Request file by path")
        print("back - return to main menu")
        print()

        choice = input("cmd> ").strip()

        if choice == "1":
            send_packet(sock, PID_LIST_FILES)

        elif choice == "2":
            send_packet(sock, PID_LATEST_IMG)

        elif choice == "3":
            path = input("Enter file path: ").strip()
            with _last_requested_file_lock:
                global _last_requested_file
                _last_requested_file = path
            send_packet(sock, PID_GET_FILE, path.encode())

        elif choice == "back":
            break

        else:
            print("Invalid choice.")


# ── Main ─────────────────────────────────────────────────────────
def main():
    gs_port = 5000
    q7_port = 5001

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
    print(f"Fragment timeout: {FRAGMENT_TIMEOUT_S}s  |  Partials saved to: {PARTIAL_DIR}/")
    print()
    print("Commands:  ping | cmd | quit")
    print("Waiting for Q7 beacon...")
    print()

    rx = threading.Thread(target=receive_loop, args=(sock, q7_port), daemon=True)
    rx.start()

    tc = threading.Thread(target=timeout_checker, args=(sock,), daemon=True)
    tc.start()

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
        elif line == "cmd":
            cmd_menu(sock)
        elif line == 'quit':
            break
        else:
            print("Unknown command. Try: ping | cmd | quit")

    sock.close()
    print("[GS] Closed.")


if __name__ == '__main__':
    main()
