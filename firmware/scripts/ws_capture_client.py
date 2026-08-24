#!/usr/bin/env python3
"""ws_capture_client.py — minimal RFC6455 WebSocket client for device smoke tests.

Connects to the ESP32-CAM WS server endpoint (/cams), expects a text hello
JSON frame first, then captures N binary JPEG frames plus periodic status
text frames. Saves each binary frame to disk and validates the JPEG SOI/EOI
markers. Stdlib-only (no third-party websocket dependency).

Usage:
  python3 ws_capture_client.py HOST [--port 80] [--path /cams]
                                    [--frames 30] [--out DIR] [--timeout 20]

Exit codes: 0 = captured requested frames; 1 = handshake/connection failure;
2 = protocol error (unexpected frame order/close).
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time

SOI = b"\xff\xd8"
EOI = b"\xff\xd9"


def _recv_exact(sock: socket.socket, n: int, deadline: float) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("recv timeout after %d bytes" % len(buf))
        sock.settimeout(remaining)
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed mid-frame")
        buf += chunk
    return bytes(buf)


def _send_masked(sock: socket.socket, payload: bytes, opcode: int) -> None:
    """Client-to-server frames MUST be masked (RFC6455 §5.3)."""
    mask = os.urandom(4)
    header = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 1 << 16:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(header + mask + masked)


def _read_frame(sock: socket.socket, deadline: float):
    """Return (fin, opcode, payload) of one wire frame."""
    b1, b2 = _recv_exact(sock, 2, deadline)
    fin = bool(b1 & 0x80)
    opcode = b1 & 0x0F
    masked = bool(b2 & 0x80)
    ln = b2 & 0x7F
    if ln == 126:
        (ln,) = struct.unpack(">H", _recv_exact(sock, 2, deadline))
    elif ln == 127:
        (ln,) = struct.unpack(">Q", _recv_exact(sock, 8, deadline))
    assert not masked, "server frames must not be masked"
    payload = _recv_exact(sock, ln, deadline) if ln else b""
    return fin, opcode, payload


def _handshake(sock: socket.socket, host: str, port: int, path: str) -> None:
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n" % (path, host, port, key)
    )
    sock.sendall(req.encode())
    resp = bytearray()
    while b"\r\n\r\n" not in resp:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("closed during handshake")
        resp += chunk
        if len(resp) > 64 * 1024:
            raise ProtocolError("handshake response too large")
    head, _, rest = bytes(resp).partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n")[0].decode(errors="replace")
    if b" 101 " not in b" " + status_line.encode() + b" ":
        raise ConnectionError("handshake rejected: %s" % status_line)
    return rest  # bytes that arrived after the handshake (already frames)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--path", default="/cams")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--out", default="ws_frames")
    ap.add_argument("--timeout", type=float, default=25.0,
                    help="overall seconds budget for the whole capture")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    try:
        sock = socket.create_connection((args.host, args.port), timeout=5.0)
    except OSError as exc:
        print("CONNECT_FAIL %s:%d — %s" % (args.host, args.port, exc))
        return 1

    deadline = time.monotonic() + args.timeout
    try:
        leftover = _handshake(sock, args.host, args.port, args.path)
        print("HANDSHAKE_OK ws://%s:%d%s" % (args.host, args.port, args.path))
    except (OSError, ConnectionError) as exc:
        print("HANDSHAKE_FAIL — %s" % exc)
        return 1

    pending = bytearray(leftover)          # bytes already buffered post-handshake
    msg_buf = bytearray()                  # fragmented-message accumulator
    msg_opcode = 0                         # opcode of the first fragment
    got_hello = False
    got_status = 0
    saved = 0
    sizes = []
    t_first = None

    def pump() -> bytes:
        """Pull one complete wire frame out of pending/socket."""
        nonlocal pending
        while True:
            if len(pending) >= 2:
                b1 = pending[0]
                need = 2
                ln = pending[1] & 0x7F
                if ln == 126:
                    need = 4
                elif ln == 127:
                    need = 10
                have_header = len(pending) >= need
                plen = -1
                if have_header and ln == 126 and len(pending) >= 4:
                    (plen,) = struct.unpack(">H", pending[2:4])
                    need = 4 + plen
                elif have_header and ln == 127 and len(pending) >= 10:
                    (plen,) = struct.unpack(">Q", pending[2:10])
                    need = 10 + plen
                elif have_header and ln < 126:
                    plen = ln
                    need = 2 + plen
                if plen >= 0 and len(pending) >= need:
                    frame = bytes(pending[:need])
                    pending = pending[need:]
                    return frame
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("capture budget exhausted")
            sock.settimeout(min(remaining, 5.0))
            chunk = sock.recv(65536)
            if not chunk:
                raise ConnectionError("device closed the connection")
            pending += chunk

    try:
        while saved < args.frames:
            frame = pump()
            b1 = frame[0]
            fin = bool(b1 & 0x80)
            opcode = b1 & 0x0F
            b2 = frame[1]
            ln = b2 & 0x7F
            off = 2
            if ln == 126:
                (ln,) = struct.unpack(">H", frame[2:4])
                off = 4
            elif ln == 127:
                (ln,) = struct.unpack(">Q", frame[2:10])
                off = 10
            payload = frame[off:off + ln]

            if opcode == 0x9:                      # ping -> pong
                _send_masked(sock, payload, 0xA)
                continue
            if opcode == 0xA:                      # unsolicited pong
                continue
            if opcode == 0x8:                      # close
                print("CLOSED_BY_PEER after %d/%d frames" % (saved, args.frames))
                return 2

            if not fin or opcode == 0x0:           # fragmented message
                if opcode != 0x0:
                    msg_opcode = opcode
                    msg_buf.clear()
                msg_buf += payload
                if fin:
                    data, op = bytes(msg_buf), msg_opcode
                    msg_buf.clear()
                else:
                    continue
            else:
                data, op = payload, opcode

            if op == 0x1:                          # text: hello / status
                try:
                    doc = json.loads(data.decode())
                except ValueError:
                    doc = {"raw": data[:120].decode(errors="replace")}
                kind = doc.get("type", "?")
                if kind == "hello":
                    got_hello = True
                    print("HELLO %s" % json.dumps(doc))
                elif kind == "status":
                    got_status += 1
                    print("STATUS #%d %s" % (got_status, json.dumps(doc)))
                else:
                    print("TEXT %s" % json.dumps(doc))
            elif op == 0x2:                        # binary JPEG frame
                ok_soi = data.startswith(SOI)
                ok_eoi = data.endswith(EOI)
                path = os.path.join(args.out, "frame_%03d.jpg" % saved)
                with open(path, "wb") as fh:
                    fh.write(data)
                now = time.monotonic()
                if t_first is None:
                    t_first = now
                sizes.append(len(data))
                saved += 1
                print("FRAME %3d len=%6d soi=%s eoi=%s -> %s"
                      % (saved, len(data), ok_soi, ok_eoi, path))
            else:
                print("PROTO unexpected opcode 0x%X" % op)
                return 2
    except (TimeoutError, ConnectionError) as exc:
        print("STOP %s" % exc)

    elapsed = (time.monotonic() - t_first) if (t_first and len(sizes) > 1) else 0
    fps = (len(sizes) / elapsed) if elapsed > 0 else 0.0
    kbps = (sum(sizes) / elapsed / 1024.0) if elapsed > 0 else 0.0
    print("--- SUMMARY ---")
    print("hello=%s status_frames=%d jpeg_frames=%d" % (got_hello, got_status, len(sizes)))
    if sizes:
        print("bytes min=%d avg=%d max=%d total=%d"
              % (min(sizes), sum(sizes) // len(sizes), max(sizes), sum(sizes)))
    print("throughput %.1f fps, %.1f KiB/s" % (fps, kbps))
    bad = [i for i in range(len(sizes))]
    valid = all(
        open(os.path.join(args.out, "frame_%03d.jpg" % i), "rb").read().startswith(SOI)
        for i in bad
    ) if sizes else False
    print("all_frames_start_with_SOI=%s" % valid)
    return 0 if (got_hello and len(sizes) == args.frames) else 2


if __name__ == "__main__":
    sys.exit(main())
