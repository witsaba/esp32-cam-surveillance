# PRD — ESP32-CAM Firmware (Leaf Node)

> **Scope:** this document covers ONLY the firmware that runs on each ESP32-CAM leaf node. Backend, frontend, and the on-wire protocol contract with the backend are summarised here only where the firmware must comply with them.

---

## TL;DR

A low-energy, self-recovering ESP32-CAM firmware that holds one persistent WebSocket to the backend, pushes JPEG frames as binary messages, accepts control commands as text messages, and **recovers automatically from every transient failure** — including the "Wi-Fi drops after a while" failure mode that today requires a physical reboot. Energy is the dominant non-functional requirement: the node must idle at Wi-Fi modem-sleep current when no client is watching, and wake the camera + WS pipeline only on demand.

---

## Quick path (first 30 minutes of a new contributor)

1. Read § Goals, § Hardware target, and § FR-1 (boot sequence).
2. Read § Architecture (module map + state machine).
3. Pick up the next milestone in § Milestones. Each milestone is a self-contained PR-sized slice.
4. When starting M1, follow **§ Build prerequisites** — every step is an `idf.py` command, the only manual file is `firmware/main/Kconfig.projbuild`.

---

## Goals

| Goal | Measurable |
|---|---|
| Survive Wi-Fi and backend outages without human intervention | Zero physical reboots in a 30-day field test |
| Recover within 10 s of a transient drop | Time from `WEBSOCKET_EVENT_DISCONNECTED` to `WEBSOCKET_EVENT_CONNECTED` ≤ 10 s |
| Bound the worst-case stall | If N reconnect attempts fail inside M minutes, the node reboots itself (soft-recovery) |
| Idle at low power when no client watches | < 5 mA average with WS connected but no active stream |
| Persist configuration across power cycles | Wi-Fi creds, backend URL, node ID survive a full power loss with no user action |
| Eliminate the captive-portal attack window after station join | softAP is torn down the moment STA gets an IP (§FR-1 step 2) |

## Non-goals (this PRD)

- Motion detection / PIR-triggered streaming (deferred — backend can poll cameras on demand).
- On-device recording to SD card (out of scope; backend handles storage).
- Multi-camera sync / timestamping (server-side concern; ESP32 has no reliable clock at boot).
- TLS / `wss://` for LAN deployments (deferred; add when traffic crosses an untrusted network).
- OTA updates (separate PRD when needed).
- ESP32-S3 / ESP32-C3 variants (different pin map and PSRAM layout; this PRD assumes AI-Thinker ESP32-CAM).

---

## Hardware target (confirmed on connected board)

Verified against `/dev/cu.usbserial-130` with `esptool.py`:

| Property | Value | Source |
|---|---|---|
| Chip | ESP32-D0WDQ6, rev v1.0, dual-core, 240 MHz, Wi-Fi + BT | `chip_id` |
| Flash | 4 MB, 3.3 V | `flash_id` |
| MAC | `c8:f0:9e:9d:50:08` | `read_mac` |
| Board class | **AI-Thinker ESP32-CAM** (inferred from chip + 4 MB flash combo) | deduction |

PSRAM presence (4 MB on the AI-Thinker module) cannot be queried via `esptool`; it will be confirmed at M3 by `esp_spiram_get_size()` printed to the log on first camera init. If PSRAM is missing, the firmware MUST log an explicit `PSRAM_REQUIRED` error and stop.

### Pin map (AI-Thinker ESP32-CAM)

| Signal | GPIO |
|---|---|
| `PWDN` | 32 (drive LOW for normal operation) |
| `RESET` | -1 (not connected; software reset) |
| `XCLK` | 0 |
| `SIOD` (SCCB SDA) | 26 |
| `SIOC` (SCCB SCL) | 27 |
| `D7` | 35 |
| `D6` | 34 |
| `D5` | 39 |
| `D4` | 36 |
| `D3` | 21 |
| `D2` | 19 |
| `D1` | 18 |
| `D0` | 5 |
| `VSYNC` | 25 |
| `HREF` | 23 |
| `PCLK` | 22 |
| Onboard flash LED | 4 |
| Boot button (PRG) | 0 |

---

## Functional requirements

### FR-1 — Boot sequence

On power-on the firmware MUST run this exact order:

1. Initialise NVS, load `config_t` from flash. If the partition is uninitialised or the schema version mismatches, fall back to defaults and mark the config as `dirty`.
2. If `config_t.wifi.ssid` is empty OR the user is holding the boot button (GPIO0 low at boot for ≥ 3 s), enter **provisioning mode**: bring up a softAP + captive portal (covered in a follow-up task; out of scope for the first cut).
3. Connect to the configured Wi-Fi (station mode) with exponential backoff. Surface progress via the status LED (see FR-7).
4. **The moment the STA interface receives an IP, the provisioning softAP MUST be torn down** (controlled by `CONFIG_FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT=y`, default on). This closes the window where an attacker on the still-up AP could access the captive portal while the device is actively streaming. The provisioning task keeps the AP alive only while station mode is still trying to join a network.
5. Initialise the camera driver with the parameters from § Camera pipeline.
6. Initialise the WebSocket client with the parameters from § WebSocket pipeline and call `esp_websocket_client_start()`.
7. Start the supervision tasks (health, capture, stream, control).
8. Hand off to the event loop. `app_main` returns.

### FR-2 — Camera pipeline (low-energy default)

| Parameter | Default (Kconfig symbol) | Reason |
|---|---|---|
| `pixel_format` | `PIXFORMAT_JPEG` (fixed) | The only sane choice on stock ESP32 — YUV/RGB lose data under Wi-Fi load (component README §"Important to Remember") |
| `frame_size` | `FRAMESIZE_QVGA` (5) — `CONFIG_FIRMWARE_CAMERA_FRAME_SIZE` | Smallest frame that still reads as a surveillance image. ~5–10 KB at quality 18 |
| `jpeg_quality` | `18` — `CONFIG_FIRMWARE_CAMERA_JPEG_QUALITY` | Sweet spot for home surveillance. Range 0–63 (lower = better). Override in `sdkconfig.defaults` for higher quality (12) or compact frames (25) |
| `fb_count` | `1` (fixed) | Half the PSRAM of `fb_count=2`. We don't need double frame rate |
| `grab_mode` | `CAMERA_GRAB_WHEN_EMPTY` (fixed) | Deterministic, blocks `fb_get` until VSYNC — easier backpressure story |
| `xclk_freq_hz` | `10_000_000` (fixed) | 10 MHz instead of the default 20 MHz — cuts LEDC power ~30 % with no visible quality loss at QVGA |

**Tuning workflow.** To change defaults, edit the `CONFIG_FIRMWARE_CAMERA_*` lines in `firmware/sdkconfig.defaults` and re-run `idf.py build`. All project tunables are defined in `firmware/main/Kconfig.projbuild` so they show up in `idf.py menuconfig` under the `ESP32-CAM Surveillance Firmware` menu as well.

The sensor control surface (`esp_camera_sensor_get()`) is reserved for runtime reconfiguration triggered by the backend (`config` text command — see § Protocol contract). The firmware MUST NOT reinitialise the driver to change resolution or quality; use the `sensor_t` setters.

### FR-3 — WebSocket pipeline

| Parameter | Default (Kconfig symbol where applicable) | Reason |
|---|---|---|
| `uri` | `ws://<backend-host>:<port>/cams/<node-id>` from NVS | Single persistent connection per the architecture diagram in the project README. Path is fixed at `/cams/`; node-id comes from `config_t.identity.node_id` |
| `transport` | `WEBSOCKET_TRANSPORT_OVER_TCP` | LAN deployment; `wss://` is a follow-up |
| `disable_auto_reconnect` | `false` | Required to address the "I have to press restart" pain point |
| `enable_close_reconnect` | `false` | A clean CLOSE means "go to sleep" — we MUST stay asleep |
| `reconnect_timeout_ms` | `CONFIG_FIRMWARE_WS_RECONNECT_INITIAL_MS` (default `2000`), grows on failure up to `CONFIG_FIRMWARE_WS_RECONNECT_CAP_MS` (default `30000`) | Start fast, then back off to avoid hammering a dead backend |
| `ping_interval_sec` | `10` | Detect half-open connections before the watchdog does |
| `pingpong_timeout_sec` | `30` | Aggressive enough to surface dead sockets within 30 s instead of the 120 s default |
| `buffer_size` | `16384` (16 KB) | Largest single send chunk; pairs with **fragmentation** (see below) |
| `network_timeout_ms` | `5000` | Anything slower than 5 s is a dead socket for live video |
| `task_stack` | `8192` | 4 KB default is too tight for TLS later; setting it now avoids a future migration |
| `task_prio` | `5` | Same as the library default; matches the WS event handler priority |

**Fragmentation policy.** A single QVGA JPEG is typically 5–10 KB but can spike to ~15 KB on busy scenes. The 16 KB `buffer_size` covers the common case in a single `esp_websocket_client_send_bin()` call; if a frame exceeds `buffer_size`, the stream task MUST split it using `send_bin_partial` + `send_cont_msg` + `send_fin` so the receiver can reassemble from `WEBSOCKET_EVENT_DATA` `payload_offset` events.

### FR-4 — Auto-reconnect with backoff (PRIORITY 1 — user pain point)

The default library behavior is: on any transport error, wait `reconnect_timeout_ms` then try again. That alone fixes most dropouts, but a dead backend can cause infinite tight-loop reconnect storms. The firmware MUST:

1. Subscribe to `WEBSOCKET_EVENT_DISCONNECTED` and `WEBSOCKET_EVENT_ERROR`.
2. Maintain a `consecutive_failures` counter, reset to zero on `WEBSOCKET_EVENT_CONNECTED`.
3. On each failed reconnect attempt, call `esp_websocket_client_set_reconnect_timeout()` to grow the delay:

   | Attempt | Delay |
   |---|---|
   | 1 | `CONFIG_FIRMWARE_WS_RECONNECT_INITIAL_MS` (default 2 s) |
   | 2 | 4 s |
   | 3 | 8 s |
   | 4 | 16 s |
   | 5+ | `CONFIG_FIRMWARE_WS_RECONNECT_CAP_MS` (default 30 s, cap) |
4. Log each transition with `ESP_LOGW` so field debugging does not require a serial cable (visible on the LED in FR-7 too).

### FR-5 — Soft-recovery

If auto-reconnect itself cannot recover the node, the firmware MUST eventually reboot rather than spin forever:

- Trigger: `CONFIG_FIRMWARE_SOFT_RECOVERY_FAILS` (default `30`) consecutive reconnect failures within a sliding window of `CONFIG_FIRMWARE_SOFT_RECOVERY_WINDOW_MIN` minutes (default `10`).
- Action: `esp_restart()`. Log the trigger reason to NVS as `last_recovery_reason` so the next boot can surface it.
- Rationale: ESP32 networking subsystems occasionally wedge after Wi-Fi credential rotation, AP reboots, or DHCP lease expiry. A clean restart is cheaper than a truck roll.

The health task (§ Architecture) owns this counter and the decision.

### FR-6 — Control plane

Text frames from the backend carry JSON commands. The minimum viable set:

| `cmd` | Body | Effect |
|---|---|---|
| `stream` | `{"on": bool, "fps": int}` | Start or stop the capture loop at the requested rate. `fps` MUST be ≥ `CONFIG_FIRMWARE_STREAM_FPS_MIN` (default 1); values below are clamped, values above the camera ceiling (≈ 15 fps at QVGA) are clamped to the ceiling |
| `config` | `{"frame_size": "QVGA"\|"VGA", "quality": int}` | Reconfigure via `sensor_t` setters; persist to NVS if `persist=true`. Allowed `quality` range is `[0, 63]`; out-of-range values are rejected with `error` |
| `sleep` | `{}` | Stop capture, call `esp_websocket_client_close()` with code `1000`. Auto-reconnect MUST NOT fire after this |
| `reboot` | `{}` | Persist dirty config, then `esp_restart()` |
| `identify` | `{}` | Reply with a text frame containing node ID, firmware version, uptime, current config |

Unknown commands MUST be logged and answered with `{"cmd":"error","reason":"unknown","id":"<original id>"}`. The control task MUST NOT block the WS event loop.

### FR-7 — Status LED and boot button

Default GPIO mapping for the AI-Thinker ESP32-CAM onboard LED (GPIO4):

| State | LED behaviour |
|---|---|
| Booting / NVS init | solid ON |
| Wi-Fi connecting | 200 ms blink |
| WS connecting | 100 ms blink |
| WS connected, idle (no stream) | 1 s heartbeat |
| WS connected, streaming | solid ON |
| Reconnect backoff active | 2 s blink (visible "I'm trying but backing off") |
| Soft-recovery about to fire | rapid blink (5 Hz) for 3 s before restart |

Boot button (GPIO0, the onboard PRG button) behaviour:

- Tap (< 100 ms) during runtime: ignored.
- Long press (≥ 3 s) at boot: enter provisioning mode (FR-1 step 2).
- Long press (≥ 10 s) at runtime: factory reset (wipe NVS `config` namespace, restart into provisioning).

---

## Non-functional requirements

### Energy budget

| Mode | Target average current | How |
|---|---|---|
| Idle (WS connected, no stream) | < 5 mA | Camera powered down, Wi-Fi modem sleep, capture task suspended |
| Streaming (QVGA @ 5 fps) | < 200 mA | Camera active, Wi-Fi active, JPEG encode in HW |
| Reconnect backoff | < 30 mA | WS task blocked in `wait_timeout`, camera off |

The firmware MUST power down the camera (`esp_camera_deinit()` or PWDN GPIO) between active streams when no client is connected.

### Memory budget

| Resource | Budget | Notes |
|---|---|---|
| Static RAM (firmware) | < 100 KB | Leaves ~190 KB for the network stack and TLS later |
| Frame queue depth | 2 frames | Backpressure: if the queue is full, the capture task MUST drop the new frame and return the buffer immediately — never block the I2S DMA |
| Command queue depth | 8 entries | Control commands are burstty but tiny |
| PSRAM for frame buffers | 1 × QVGA JPEG slot in PSRAM | Configured via `fb_location = CAMERA_FB_IN_PSRAM` |

### Reliability targets

| Metric | Target |
|---|---|
| MTBF (mean time between failures requiring reboot) | ≥ 30 days |
| Recovery time from transient drop | ≤ 10 s |
| Recovery time from backend outage | ≤ 30 s after backend returns |
| Worst-case stall before self-recovery | 10 minutes |
| Configuration corruption tolerance | Auto-recover from any single-bit NVS error via schema-version check |

### Security posture (this PRD)

- Plain `ws://` is acceptable for LAN-only deployments.
- The firmware MUST validate any incoming JSON command against a strict allow-list of `cmd` strings before acting on it.
- The firmware MUST NOT expose a network-facing admin interface in this PRD scope.
- The provisioning softAP MUST be torn down the moment station mode joins a network (FR-1 step 4).

---

## Architecture

### Module map

```
firmware/
├── main/                      # app_main + boot orchestration
├── modules/
│   ├── config/                # NVS read/write of config_t
│   ├── wifi/                  # station connect + backoff
│   ├── camera/                # esp32-camera init + sensor accessors
│   ├── ws/                    # esp_websocket_client wrapper + event handler
│   ├── stream/                # capture task → frame queue → WS send task
│   ├── control/               # incoming text frames → command dispatch
│   ├── health/                # LED, watchdog, soft-recovery counter
│   └── provisioning/          # softAP + captive portal (follow-up)
```

### Task and queue topology

```
            ┌──────────────────────────────────────────────────────┐
            │                       app_main                        │
            │  init NVS → load config → wifi → cam → ws → start    │
            └─────────────┬────────────────────────────────────────┘
                          │
                          ▼
   ┌────────────┐    ┌─────────────┐    ┌─────────────┐
   │ capture    │──▶ │ frame_queue │──▶ │ stream      │ ── WS binary
   │ task       │    │ (depth 2)   │    │ task        │
   └────────────┘    └─────────────┘    └─────────────┘
                          │
                          │ overflow → drop + ESP_LOGW
                          ▼
   ┌────────────┐    ┌─────────────┐    ┌─────────────┐
   │ WS event   │──▶ │ cmd_queue   │──▶ │ control     │ → sensor_t
   │ handler    │    │ (depth 8)   │    │ task        │ → config_t
   └─────────────┘    └─────────────┘    └─────────────┘
                                                  │
                                                  ▼
                                          ┌─────────────┐
                                          │ health task │ ← watchdog
                                          │             │ → LED + reboot
                                          └─────────────┘
```

### Connection state machine

```
                ┌────────────┐
                │   INIT     │  (config loaded, cam + wifi init)
                └─────┬──────┘
                      ▼
                ┌────────────┐
        ┌──────▶│  NOLINK    │  (wifi down OR no config)
        │       └─────┬──────┘
        │             ▼
        │       ┌────────────┐
        │       │ CONNECTING │  (wifi up, WS handshake)
        │       └─────┬──────┘
        │             ▼
        │       ┌────────────┐      frame available
        │       │ CONNECTED  │ ─────────────────────▶ STREAMING
        │       │   (idle)   │ ◀─────────────────────
        │       └─────┬──────┘   stream off / no client
        │             │
        │       DISCONNECTED / ERROR
        │             ▼
        │       ┌────────────┐
        │       │ BACKOFF    │  delay = f(failures), max 30 s
        │       └─────┬──────┘
        │             │ retry
        └─────────────┘
                      │ 30 fails in 10 min
                      ▼
                ┌────────────┐
                │ REBOOTING  │  esp_restart() + log last reason
                └────────────┘
```

A clean CLOSE frame received while in `CONNECTED` or `STREAMING` transitions directly to `NOLINK` (no backoff, no reconnect) — this is the "go to sleep" path.

### Module APIs (interfaces, not signatures)

| Module | Exports |
|---|---|
| `config` | `config_load(config_t *out)`, `config_save(const config_t *in)`, `config_factory_reset()` |
| `wifi` | `wifi_connect_async()`, `wifi_is_connected()`, `wifi_wait_connected(timeout_ms)` |
| `camera` | `camera_init()`, `camera_deinit()`, `camera_capture_blocking(camera_fb_t **fb)`, `sensor_handle()` (returns `sensor_t*`) |
| `ws` | `ws_send_frame(const uint8_t *buf, size_t len)`, `ws_send_text(const char *json)`, `ws_close_clean()`, `ws_is_connected()`, `ws_register_text_handler(cb)` |
| `stream` | `stream_start(fps)`, `stream_stop()`, `stream_set_enabled(bool)` |
| `control` | `control_init()` (creates the control task; everything else is internal) |
| `health` | `health_init()` (creates the health task) |

All queues and task handles are file-scope statics inside each module — no global FreeRTOS soup.

---

## Protocol contract (firmware-side view)

The firmware is the data-plane peer. The backend terminates the connection and fans out to clients. The contract:

**Outbound (camera → backend):**

| Frame type | Payload | When |
|---|---|---|
| Binary | Raw JPEG bytes, single message | Every frame while streaming |
| Text | `{"type":"hello","id":"<node-id>","fw":"<version>","caps":["jpeg","stream","identify"]}` | Once on `WEBSOCKET_EVENT_CONNECTED` |
| Text | `{"type":"status","uptime_s":N,"rssi_dbm":N,"free_heap":N,"fb_drops":N,"reconnects":N}` | Every 30 s while connected |
| Text | `{"type":"identify_ok",...}` | Reply to `identify` command |

**Inbound (backend → camera):**

Text frames only — see FR-6.

**Heartbeat discipline:** the firmware MUST respond to any backend ping with a pong within 5 s (the library does this automatically once `pingpong_timeout_sec` is configured — we just need to keep it from going dormant).

---

## Milestones

| ID | Scope | Acceptance |
|---|---|---|
| **M0** | Validation scaffold (see commit history of branch `docs/esp32-cam-firmware-prd` for proof artifacts) | `idf.py build` succeeds with both managed components, `firmware.bin` < 256 KB |
| **M1** | Boot + NVS config + LED + boot button | Cold-boot reaches `NOLINK`; NVS round-trip works; LED reflects state; 10-s button press triggers factory reset |
| **M2** | Wi-Fi station with backoff | Connects to a known SSID; recovers from AP reboot within 30 s |
| **M3** | Camera capture + frame queue + QVGA loopback | `fb_get` / `fb_return` cycle sustains 5 fps under no WS load; PSRAM allocation visible in heap |
| **M4** | WebSocket client + auto-reconnect + soft-recovery | Manual Wi-Fi kill mid-stream triggers reconnect within 10 s; 30-fail threshold triggers `esp_restart()`; `last_recovery_reason` visible after restart |
| **M5** | Control plane (FR-6 commands) | Backend `stream on`/`off` and `config` work; unknown commands echoed as `error` |
| **M6** | Energy profiling + sleep mode | Idle current < 5 mA measured with a uCurrent or equivalent |

Each milestone ends with a `idf.py build` that fits in flash and a manual smoke test on hardware. CI automation is a follow-up.

---

## Build prerequisites

This PR is docs-only. The M1 implementer recreates the ESP-IDF project using **`idf.py` commands** — no hand-written `CMakeLists.txt`, `idf_component.yml`, `sdkconfig.defaults`, or `.gitignore`. The only file that must be authored by hand is `firmware/main/Kconfig.projbuild`, because `idf.py` has no command for project-level Kconfig symbols.

> If any step below emits `unknown kconfig symbol 'FIRMWARE_*'`, the `Kconfig.projbuild` file is misplaced — confirm it lives at `firmware/main/Kconfig.projbuild`, **not** at `firmware/`.

### Step 1 — Bootstrap the project from the camera example

The camera example ships with the AI-Thinker pin map already correct, so we start there instead of `idf.py create-project` from a blank template.

```bash
# From the repo root
cd firmware
idf.py create-project-from-example "espressif/esp32-camera:camera_example"
# This drops camera_example/ inside firmware/. Move its contents up one level:
mv camera_example/* camera_example/.* . 2>/dev/null
rmdir camera_example
```

### Step 2 — Add the WebSocket client

`idf.py add-dependency` updates `idf_component.yml` automatically — do NOT hand-edit it.

```bash
idf.py add-dependency "espressif/esp_websocket_client"
idf.py add-dependency "espressif/esp32-camera"
```

Pinning to a specific version (recommended for reproducibility):

```bash
idf.py add-dependency "espressif/esp_websocket_client==1.8.0"
idf.py add-dependency "espressif/esp32-camera==2.1.7"
```

### Step 3 — Set the chip target

```bash
idf.py set-target esp32
```

This populates `sdkconfig` from `sdkconfig.defaults`. If the example doesn't ship a `sdkconfig.defaults`, create one with `idf.py save-defconfig` after the menuconfig session in Step 5.

### Step 4 — Author the project Kconfig (manual file)

Create **`firmware/main/Kconfig.projbuild`** with the project tunables referenced throughout this PRD. `idf.py` does not generate this — it has to be hand-written. It must live in the project component (where `main.c` is), not at the project root.

```kconfig
menu "ESP32-CAM Surveillance Firmware"

    config FIRMWARE_CAMERA_JPEG_QUALITY
        int "JPEG quality (0=highest, 63=lowest, 18=balanced)"
        range 0 63
        default 18
        help
            OV2640 JPEG quality factor. Lower = higher quality + larger frame.
            12 ≈ premium, 18 ≈ balanced (default), 25 ≈ compact for low bandwidth.

    config FIRMWARE_CAMERA_FRAME_SIZE
        int "Frame size enum (FRAMESIZE_QVGA=5, VGA=8, SVGA=9)"
        range 0 13
        default 5
        help
            See esp32-camera framesize_t. Low-energy default is QVGA (5).

    config FIRMWARE_STREAM_FPS
        int "Default stream framerate when backend sends stream.on"
        range 1 30
        default 5

    config FIRMWARE_STREAM_FPS_MIN
        int "Hard floor on stream fps"
        range 1 10
        default 1

    config FIRMWARE_WS_RECONNECT_INITIAL_MS
        int "Reconnect delay after the first failure (ms)"
        range 500 30000
        default 2000

    config FIRMWARE_WS_RECONNECT_CAP_MS
        int "Maximum reconnect delay after exponential growth (ms)"
        range 5000 300000
        default 30000

    config FIRMWARE_SOFT_RECOVERY_FAILS
        int "Consecutive reconnect failures before esp_restart()"
        range 5 200
        default 30

    config FIRMWARE_SOFT_RECOVERY_WINDOW_MIN
        int "Sliding window for counting soft-recovery failures (minutes)"
        range 1 60
        default 10

    config FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT
        bool "Power down the softAP once station mode joins a network"
        default y

endmenu
```

### Step 5 — Configure PSRAM and project tunables via menuconfig

Use `idf.py menuconfig` to set the values referenced throughout this PRD. Then capture the result with `idf.py save-defconfig` — that command writes **only the non-default options** into `sdkconfig.defaults`. Do NOT hand-author the file; always let `idf.py save-defconfig` generate it.

```bash
idf.py menuconfig
# In the TUI:
#   Serial flasher config → Flash size → 4 MB
#   Serial flasher config → Flash frequency → 80 MHz
#   Component config → ESP PSRAM → Enable PSRAM
#   Component config → ESP PSRAM → Speed → 80 MHz
#   Component config → ESP PSRAM → Try to allocate Wi-Fi/LWIP in PSRAM
#   Component config → ESP WebSocket Client → Enable dynamic buffer
#   ESP32-CAM Surveillance Firmware → set CONFIG_FIRMWARE_* values
#   <Save> and <Exit>

idf.py save-defconfig
# Generates/updates sdkconfig.defaults from the non-default options.
```

### Step 6 — Build, flash, monitor

```bash
idf.py build                      # compiles; first build fetches managed_components
idf.py -p /dev/cu.usbserial-XXXX flash
idf.py -p /dev/cu.usbserial-XXXX monitor
```

Repeat as needed during development:

| Need | Command |
|---|---|
| Pull a newer version of a managed component | `idf.py update-dependencies` |
| Re-fetch components without changing versions | delete `managed_components/` + `dependencies.lock`, then `idf.py build` |
| Re-run CMake after editing `CMakeLists.txt` / `Kconfig.projbuild` | `idf.py reconfigure` |
| Delete build output but keep `sdkconfig` | `idf.py clean` |
| Delete build output AND `sdkconfig` | `idf.py fullclean` |
| Inspect binary size | `idf.py size` / `idf.py size-components` / `idf.py size-files` |
| Interactive Kconfig editor | `idf.py menuconfig` |
| Regenerate `sdkconfig.defaults` from current `sdkconfig` | `idf.py save-defconfig` |
| Erase flash before re-flashing | `idf.py erase-flash` |

### Why this matters

The first cut of this PRD shipped hand-written `CMakeLists.txt`, `idf_component.yml`, `sdkconfig.defaults`, and `.gitignore` files. That is brittle — these are scaffolding artefacts that `idf.py` generates and maintains for you. The only thing `idf.py` cannot generate is the project-level Kconfig, which is why `Kconfig.projbuild` is the sole manual file. Hand-authoring the others invites drift between the file and what `idf.py` would have produced.

---

## Open questions

All five open questions from the early draft are resolved:

1. ✅ **Hardware confirmed.** Connected board on `/dev/cu.usbserial-130` is **ESP32-D0WDQ6 + 4 MB flash** → AI-Thinker ESP32-CAM. Pin map in § Hardware target. PSRAM will be confirmed at runtime in M3.
2. ✅ **JPEG quality configurable.** `CONFIG_FIRMWARE_CAMERA_JPEG_QUALITY` (default `18`). Override in `firmware/sdkconfig.defaults` or via `idf.py menuconfig`. Runtime override via the `config` text command also bounded to `[0, 63]`.
3. ✅ **Default stream fps = 5, configurable.** `CONFIG_FIRMWARE_STREAM_FPS` (default `5`). Runtime override via the `stream` text command; clamped to `[CONFIG_FIRMWARE_STREAM_FPS_MIN, hardware_ceiling]`.
4. ✅ **Backend URL shape confirmed.** `ws://<host>:<port>/cams/<node-id>`.
5. ✅ **Provisioning via softAP + captive portal.** Plus the security constraint `CONFIG_FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT=y` — softAP is torn down the instant station mode joins a network.

---

## Validation status

The validation scaffold (`idf.py set-target esp32 && idf.py build` against the pin map in § Hardware target) was executed and green before this PR was scoped down to docs-only. The proof artifacts (`managed_components/`, `build/firmware.bin`) are not part of this PR; the implementer reproduces them in M1 using the files in § Build prerequisites.

| Check | Result | Evidence |
|---|---|---|
| `idf.py --version` | ESP-IDF v5.5.3 | local shell |
| `idf.py set-target esp32` | OK | local CMake configure |
| `idf.py build` | OK — `firmware.bin` 222 KB, 79 % flash free | build log (see branch commit history) |
| `espressif/esp32-camera 2.1.7` fetched | OK | `managed_components/espressif__esp32-camera/` |
| `espressif/esp_websocket_client 1.8.0` fetched | OK | `managed_components/espressif__esp_websocket_client/` |
| Project Kconfig (`firmware/main/Kconfig.projbuild`) loaded | OK — 9 `CONFIG_FIRMWARE_*` symbols visible in `sdkconfig`, no unknown-symbol warnings | `sdkconfig` |
| `sdkconfig.defaults` overrides parse cleanly | OK | `sdkconfig` |
| Connected board probed | ESP32-D0WDQ6, 4 MB flash, MAC `c8:f0:9e:9d:50:08` on `/dev/cu.usbserial-130` | `esptool.py` |
| PSRAM presence | **Pending** — confirmed at runtime in M3 via `esp_spiram_get_size()` | — |
| Hardware flash + monitor | **Pending** — run `idf.py -p /dev/cu.usbserial-130 flash monitor` when M1 lands | — |