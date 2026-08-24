#!/usr/bin/env python3
"""ws_stream_view.py — interactive WebSocket stream viewer for the ESP32-CAM.

Asks for the device IP (Enter accepts the default), connects to the /cams
WebSocket endpoint, prints the hello/status identity frames, then streams
binary JPEG frames indefinitely with a live one-line progress meter.
Ctrl+C stops cleanly with a session summary.

Stdlib-only; low-level RFC6455 helpers are shared with ws_capture_client.py.

Usage:
  python3 ws_stream_view.py                 # asks for IP (default below)
  python3 ws_stream_view.py 192.168.1.48    # skip the prompt
  python3 ws_stream_view.py --out frames/   # also save each JPEG

Exit codes: 0 = clean stop (Ctrl+C); 1 = connect/handshake failure;
2 = protocol error.
"""
import argparse
import json
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_capture_client import SOI, EOI, _handshake, _read_frame, _send_masked  # noqa: E402

DEFAULT_IP = "192.168.1.48"


def _ask_for_ip(argv_host: str) -> str:
    """Positional arg wins; otherwise prompt with a default."""
    if argv_host:
        return argv_host
    try:
        raw = input("Device IP [%s]: " % DEFAULT_IP).strip()
    except EOFError:
        raw = ""
    return raw or DEFAULT_IP


def main() -> int:
    ap = argparse.ArgumentParser(description="Stream JPEG frames from an ESP32-CAM WS server.")
    ap.add_argument("host", nargs="?", default="", help="device IP (omit to be prompted)")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--path", default="/cams")
    ap.add_argument("--out", default="", help="optional dir to save every JPEG frame")
    ap.add_argument("--connect-timeout", type=float, default=8.0,
                    help="seconds budget for TCP+WS handshake")
    ap.add_argument("--duration", type=float, default=0,
                    help="stop after N seconds of streaming (0 = until Ctrl+C)")
    args = ap.parse_args()

    ip = _ask_for_ip(args.host)

    try:
        sock = socket.create_connection((ip, args.port), timeout=args.connect_timeout)
    except OSError as exc:
        print("✗ CONNECT_FAIL %s:%d — %s" % (ip, args.port, exc))
        return 1

    deadline = time.monotonic() + args.connect_timeout
    try:
        leftover = _handshake(sock, ip, args.port, args.path)
        print("✓ Connected ws://%s:%d%s" % (ip, args.port, args.path))
    except (OSError, ConnectionError) as exc:
        print("✗ HANDSHAKE_FAIL — %s" % exc)
        return 1

    out_dir = ""
    if args.out:
        out_dir = args.out
        os.makedirs(out_dir, exist_ok=True)

    pending = bytearray(leftover)
    msg_buf = bytearray()
    msg_opcode = 0
    got_hello = False
    status_count = 0
    frames = 0
    total_bytes = 0
    t_start = None
    t_last_frame = None
    fps_window = []          # (timestamp, ) of last frames for smooth fps

    def pump() -> bytes:
        nonlocal pending
        while True:
            if len(pending) >= 2:
                b1 = pending[0]
                ln = pending[1] & 0x7F
                need = 2
                plen = -1
                if ln == 126:
                    need = 4
                    if len(pending) >= 4:
                        (plen,) = struct.unpack(">H", pending[2:4])
                        need = 4 + plen
                elif ln == 127:
                    need = 10
                    if len(pending) >= 10:
                        (plen,) = struct.unpack(">Q", pending[2:10])
                        need = 10 + plen
                else:
                    plen = ln
                    need = 2 + plen
                if plen >= 0 and len(pending) >= need:
                    frame = bytes(pending[:need])
                    pending = pending[need:]
                    return frame
            chunk = sock.recv(65536)
            if not chunk:
                raise ConnectionError("device closed the connection")
            pending += chunk

    rc = 0
    stop_at = (time.monotonic() + args.duration) if args.duration > 0 else None
    try:
        while True:
            if stop_at is not None and time.monotonic() >= stop_at:
                print("\n■ duration reached")
                break
            frame = pump()
            b1 = frame[0]
            fin = bool(b1 & 0x80)
            opcode = b1 & 0x0F
            ln = frame[1] & 0x7F
            off = 2
            if ln == 126:
                (ln,) = struct.unpack(">H", frame[2:4])
                off = 4
            elif ln == 127:
                (ln,) = struct.unpack(">Q", frame[2:10])
                off = 10
            payload = frame[off:off + ln]

            if opcode == 0x9:                       # ping -> pong
                _send_masked(sock, payload, 0xA)
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x8:
                print("\n✗ Closed by peer after %d frames" % frames)
                rc = 2
                break

            if not fin or opcode == 0x0:            # fragmented message
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

            now = time.monotonic()

            if op == 0x1:                           # text: hello / status
                try:
                    doc = json.loads(data.decode())
                except ValueError:
                    doc = {"type": "?", "raw": data[:80].decode(errors="replace")}
                kind = doc.get("type", "?")
                if kind == "hello":
                    got_hello = True
                    print("✓ hello   name=%r mac=%s fw=%s caps=%s"
                          % (doc.get("name"), doc.get("mac"),
                             doc.get("fw"), ",".join(doc.get("caps", []))))
                elif kind == "status":
                    status_count += 1
                    print("ℹ status  #%d %s" % (status_count, json.dumps(doc)))
                else:
                    print("ℹ text    %s" % json.dumps(doc))
            elif op == 0x2:                         # binary JPEG frame
                valid = data.startswith(SOI) and data.endswith(EOI)
                frames += 1
                total_bytes += len(data)
                if t_start is None:
                    t_start = now
                fps_window.append(now)
                while fps_window and now - fps_window[0] > 5.0:
                    fps_window.pop(0)
                inst_fps = (len(fps_window) / (now - fps_window[0])
                            if len(fps_window) > 1 else 0.0)
                elapsed = now - t_start
                avg_fps = frames / elapsed if elapsed > 0 else 0.0
                t_last_frame = now
                marker = "" if valid else "  ⚠ BAD JPEG MARKERS"
                sys.stdout.write(
                    "\r▶ frame %-6d %6d B | inst %4.1f fps | avg %4.1f fps "
                     "| %7.1f KiB/s%s\033[K"
                    % (frames, len(data), inst_fps, avg_fps,
                       total_bytes / elapsed / 1024.0 if elapsed > 0 else 0.0,
                       marker))
                sys.stdout.flush()
                if out_dir:
                    path = os.path.join(out_dir, "frame_%05d.jpg" % frames)
                    with open(path, "wb") as fh:
                        fh.write(data)
            else:
                print("\n✗ unexpected opcode 0x%X" % op)
                rc = 2
                break
    except KeyboardInterrupt:
        print("\n■ stopped by user")
    except (TimeoutError, ConnectionError, OSError) as exc:
        print("\n✗ STOP %s" % exc)
        rc = 1 if frames == 0 else rc

    sock.close()
    print("--- SESSION SUMMARY ---")
    print("device     : %s:%d%s" % (ip, args.port, args.path))
    print("hello      : %s | status frames: %d" % (got_hello, status_count))
    print("jpeg frames: %d (%d bytes total)" % (frames, total_bytes))
    if t_start and t_last_frame and t_last_frame > t_start:
        el = t_last_frame - t_start
        print("throughput : %.1f fps avg, %.1f KiB/s"
              % (frames / el if el > 0 else 0.0,
                 total_bytes / el / 1024.0 if el > 0 else 0.0))
    return rc


if __name__ == "__main__":
    sys.exit(main())
