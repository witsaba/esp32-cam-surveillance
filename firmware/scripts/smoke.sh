#!/usr/bin/env bash
# scripts/smoke.sh — FW-02 + FW-03 device-side smoke.
#
# Builds the firmware, flashes it to the connected ESP32-CAM, captures the
# first `${TIMEOUT_S}` seconds of monitor output, and asserts that the
# log lines emitted by `firmware/components/boot/boot.c::boot_run()`
# (via the orchestrator's stubs) are present. The FW-02.4 lines are
# kept for the smoke regression; the FW-03 lines assert the boot
# orchestrator reached the green path (or its provisioning branch).
#
# Expected lines (all must be present for PASS):
#   - `fw: boot_run ret=ok step=return` — green path completion
#   - `stub: wifi_init`                   — wifi init stub fired
#   - `stub: camera_init`                 — camera init stub fired
#   - `stub: health_task_start`           — supervision stub fired
#
# Usage:
#   scripts/smoke.sh [PORT] [TIMEOUT_SECONDS] [LOG_FILE]
#
# Defaults:
#   PORT=/dev/cu.usbserial-130
#   TIMEOUT_SECONDS=8
#   LOG_FILE=build/smoke.log
#
# Exit codes:
#   0 — PASS (expected lines observed)
#   1 — FAIL (build/flash/monitor error, or expected line missing)
#   2 — usage error

set -euo pipefail

PORT="${1:-/dev/cu.usbserial-130}"
TIMEOUT_S="${2:-8}"
LOG_FILE="${3:-$(dirname "$0")/../build/smoke.log}"

# --- pre-flight -----------------------------------------------------------
if [ ! -e "$PORT" ]; then
  printf '[smoke] FAIL: serial port %s does not exist\n' "$PORT" >&2
  printf '[smoke] Plug in the ESP32-CAM board or override PORT.\n' >&2
  exit 1
fi

export IDF_PATH="${IDF_PATH:-$HOME/.espressif/v5.5.3/esp-idf}"
if [ ! -d "$IDF_PATH" ]; then
  printf '[smoke] FAIL: IDF_PATH=%s does not exist\n' "$IDF_PATH" >&2
  printf '[smoke] Set IDF_PATH to the ESP-IDF v5.5.3 install directory.\n' >&2
  exit 1
fi

# shellcheck disable=SC1090,SC1091
source "$IDF_PATH/export.sh" >/dev/null 2>&1

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

mkdir -p "$(dirname "$LOG_FILE")"

# --- (1/3) build ----------------------------------------------------------
printf '[smoke] (1/3) building firmware...\n'
if ! idf.py build >/dev/null; then
  printf '[smoke] FAIL: idf.py build failed\n' >&2
  exit 1
fi

# --- (2/3) flash ---------------------------------------------------------
# Flash is run synchronously and without a wall-clock budget because the
# 232 KB app binary takes ~8-12 s to write over the USB-serial adapter.
# A bounded background process + SIGTERM pattern (used below for monitor)
# would race the write and leave the device in a half-flashed state.
printf '[smoke] (2/3) flashing firmware to %s...\n' "$PORT"
if ! idf.py -p "$PORT" flash; then
  printf '[smoke] FAIL: idf.py flash failed\n' >&2
  exit 1
fi

# --- (3/3) monitor capture ---------------------------------------------
# We bypass `idf.py monitor` because it requires a real TTY on stdin
# (refuses to run with stdin redirected). Instead, scripts/capture_monitor.py
# opens the serial port directly via pyserial and writes each line to
# the log file. The script runs in the IDF Python env which has pyserial
# installed.
printf '[smoke] (3/3) capturing monitor output for %ss -> %s\n' "$TIMEOUT_S" "$LOG_FILE"
# Small settle so the device's reboot triggered by `idf.py flash` lands
# cleanly before pyserial opens the port (otherwise the open can race
# the boot output and miss the very first lines).
sleep 1
if ! python "$PROJECT_DIR/scripts/capture_monitor.py" "$PORT" "$TIMEOUT_S" "$LOG_FILE"; then
  printf '[smoke] FAIL: monitor capture script exited non-zero\n' >&2
  exit 1
fi

# --- verdict -------------------------------------------------------------
if ! grep -q 'fw:' "$LOG_FILE"; then
  printf '[smoke] FAIL: no "fw:" log lines observed in %s\n' "$LOG_FILE" >&2
  printf '[smoke] --- last 30 lines of captured output ---\n' >&2
  tail -30 "$LOG_FILE" >&2 || true
  exit 1
fi

if ! grep -q 'boot_run ret=ok step=return' "$LOG_FILE"; then
  printf '[smoke] FAIL: boot_run green-path log line missing in %s\n' "$LOG_FILE" >&2
  printf '[smoke] --- captured boot/stub lines ---\n' >&2
  grep -E '(fw: boot|boot: provisioning)' "$LOG_FILE" >&2 || true
  exit 1
fi

if ! grep -q 'stub: wifi_init' "$LOG_FILE"; then
  printf '[smoke] FAIL: wifi_init stub log line missing in %s\n' "$LOG_FILE" >&2
  printf '[smoke] --- captured boot/stub lines ---\n' >&2
  grep -E 'stub:' "$LOG_FILE" >&2 || true
  exit 1
fi

if ! grep -q 'stub: camera_init' "$LOG_FILE"; then
  printf '[smoke] FAIL: camera_init stub log line missing in %s\n' "$LOG_FILE" >&2
  printf '[smoke] --- captured boot/stub lines ---\n' >&2
  grep -E 'stub:' "$LOG_FILE" >&2 || true
  exit 1
fi

if ! grep -q 'stub: health_task_start' "$LOG_FILE"; then
  printf '[smoke] FAIL: health_task_start stub log line missing in %s\n' "$LOG_FILE" >&2
  printf '[smoke] --- captured boot/stub lines ---\n' >&2
  grep -E 'stub:' "$LOG_FILE" >&2 || true
  exit 1
fi

printf '[smoke] PASS: FW-02 + FW-03 boot-sequence log lines observed\n'
printf '[smoke] --- captured lines ---\n'
grep 'fw:' "$LOG_FILE" | sed 's/^/    /'
exit 0
