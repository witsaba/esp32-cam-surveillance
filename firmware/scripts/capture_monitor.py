#!/usr/bin/env python3
"""capture_monitor.py — minimal serial-port capture for the FW-02 smoke.

Reads raw bytes from the given serial port at 115200 baud for the given
duration, decodes them as UTF-8 (with replacement on bad bytes), and
writes them line-by-line to the given log file.

Why not `idf.py monitor`? idf_monitor.py (via prompt_toolkit) requires a
real TTY on stdin for keyboard input; it refuses to run when stdin is
redirected, which is the case when this script is invoked from a CI
pipeline or a smoke wrapper. For the FW-02 smoke we only need raw serial
output — symbol decoding is not required.

Usage:
    capture_monitor.py <PORT> <DURATION_SECONDS> <LOG_FILE>
"""

import sys
import time

try:
    import serial  # pyserial — included in the ESP-IDF python_env
except ImportError:
    print(f"[capture_monitor] FAIL: pyserial not available in this Python env", file=sys.stderr)
    print(f"[capture_monitor] python = {sys.executable}", file=sys.stderr)
    sys.exit(2)


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <PORT> <DURATION_SECONDS> <LOG_FILE>", file=sys.stderr)
        return 2

    port = sys.argv[1]
    duration_s = float(sys.argv[2])
    log_file = sys.argv[3]

    try:
        ser = serial.Serial(port, baudrate=115200, timeout=0.5)
    except (serial.SerialException, OSError) as exc:
        print(f"[capture_monitor] FAIL: cannot open {port}: {exc}", file=sys.stderr)
        return 1

    print(f"[capture_monitor] reading from {port} for {duration_s}s -> {log_file}")

    deadline = time.monotonic() + duration_s
    line_count = 0
    try:
        with open(log_file, "w", encoding="utf-8") as logf:
            while time.monotonic() < deadline:
                try:
                    raw = ser.readline()
                except serial.SerialException as exc:
                    print(f"[capture_monitor] serial read error: {exc}", file=sys.stderr)
                    break
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                logf.write(line + "\n")
                logf.flush()
                line_count += 1
    finally:
        ser.close()

    print(f"[capture_monitor] captured {line_count} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
