#!/usr/bin/env python3
"""Small end-to-end checker for the backend camera relay.

It asks the backend for online cameras, opens one viewer WebSocket per camera,
and saves a few raw JPEGs from each stream. It is intentionally a verifier,
not a CCTV UI.

Usage:
  python3 backend/scripts/verify_streams.py
  python3 backend/scripts/verify_streams.py --host 192.168.1.10 --frames 10
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.request


SOI = b"\xff\xd8"
EOI = b"\xff\xd9"


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("backend closed the connection")
        data += chunk
    return bytes(data)


def send_masked(sock, payload, opcode):
    mask = os.urandom(4)
    length = len(payload)
    if length < 126:
        header = bytes([0x80 | opcode, 0x80 | length])
    elif length < 1 << 16:
        header = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack(">H", length)
    else:
        header = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack(">Q", length)
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    sock.sendall(header + mask + masked)


def read_frame(sock):
    first, second = read_exact(sock, 2)
    length = second & 0x7F
    if length == 126:
        (length,) = struct.unpack(">H", read_exact(sock, 2))
    elif length == 127:
        (length,) = struct.unpack(">Q", read_exact(sock, 8))
    if second & 0x80:
        raise ValueError("backend sent a masked frame")
    return first & 0x0F, read_exact(sock, length) if length else b""


def connect_viewer(host, port, mac, timeout):
    path = "/api/cameras/%s/stream" % mac
    sock = socket.create_connection((host, port), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
        % (path, host, port, key)
    )
    sock.sendall(request.encode())
    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("backend closed during handshake")
        response += chunk
        if len(response) > 64 * 1024:
            raise ValueError("handshake response too large")
    head = bytes(response).split(b"\r\n", 1)[0]
    if b" 101 " not in b" " + head + b" ":
        raise ConnectionError(head.decode(errors="replace"))
    return sock


def capture_camera(host, port, mac, frames, duration, output):
    started = time.monotonic()
    saved = 0
    bad = 0
    camera_dir = os.path.join(output, mac)
    os.makedirs(camera_dir, exist_ok=True)
    try:
        sock = connect_viewer(host, port, mac, 5.0)
        sock.settimeout(2.0)
        while saved < frames and time.monotonic() - started < duration:
            opcode, payload = read_frame(sock)
            if opcode == 0x9:
                send_masked(sock, payload, 0xA)
            elif opcode == 0x1:
                meta = json.loads(payload.decode())
                if meta.get("type") != "stream_meta":
                    raise ValueError("unexpected text frame: %s" % meta)
            elif opcode == 0x2:
                if not (payload.startswith(SOI) and payload.endswith(EOI)):
                    bad += 1
                with open(os.path.join(camera_dir, "frame_%03d.jpg" % saved), "wb") as stream:
                    stream.write(payload)
                saved += 1
            elif opcode == 0x8:
                break
            else:
                raise ValueError("unexpected WebSocket opcode 0x%x" % opcode)
        sock.close()
        return saved, bad, None
    except Exception as exc:  # one camera failure must not hide other results
        return saved, bad, str(exc)


def main():
    parser = argparse.ArgumentParser(description="Verify backend camera relay streams")
    parser.add_argument("--host", default="127.0.0.1", help="backend HTTP host")
    parser.add_argument("--port", type=int, default=8080, help="backend HTTP port")
    parser.add_argument("--frames", type=int, default=5, help="JPEGs required per camera")
    parser.add_argument("--duration", type=float, default=20.0, help="seconds per camera")
    parser.add_argument("--out", default="stream_verify_frames", help="output directory")
    args = parser.parse_args()

    url = "http://%s:%d/api/cameras?status=online" % (args.host, args.port)
    with urllib.request.urlopen(url, timeout=5.0) as response:
        cameras = json.load(response).get("cameras", [])
    if not cameras:
        print("No online cameras found.")
        return 2

    results = {}
    threads = []

    def run(camera):
        mac = camera["mac"]
        results[mac] = capture_camera(args.host, args.port, mac, args.frames, args.duration, args.out)

    for camera in cameras:
        thread = threading.Thread(target=run, args=(camera,), daemon=True)
        threads.append(thread)
        thread.start()
    for thread in threads:
        thread.join()

    failed = False
    for mac in sorted(results):
        saved, bad, error = results[mac]
        state = "OK" if not error and saved == args.frames and bad == 0 else "FAIL"
        failed |= state == "FAIL"
        detail = " error=%s" % error if error else ""
        print("%s %s frames=%d bad_jpeg=%d%s" % (state, mac, saved, bad, detail))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
