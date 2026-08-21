# ESP32 Cam Surveillance

Low-energy ESP32 camera surveillance network for **home and business** use.

A monorepo containing the firmware that runs on each ESP32 camera, the
backend that aggregates the streams, and the dashboard that lets you watch
everything live.

## Goals

- **Low energy consumption per node** — ESP-IDF power management, sleep modes
  between frames, on-demand wake.
- **Many cameras on a single network** — designed to scale to dozens of nodes
  on a LAN or VPN.
- **WebSockets end-to-end** — cameras stream MJPEG / H.264 / raw frames over
  WebSockets to the backend, which fans them out to the front-end.
- **Self-hostable** — no cloud dependency, run the server on a Raspberry Pi,
  a NAS, or a small VPS.
- **Open source** — Apache License 2.0.

## Architecture

```
+-------------+        WebSocket        +-----------+       WebSocket / HTTP       +-----------+
|  ESP32 cam  |  ------------------->   |  Go       |  ------------------------->    |  Qwik     |
|  (ESP-IDF)  |    JPEG / H.264 frames  |  backend  |     fan-out + REST API       |  frontend |
+-------------+                          +-----------+                               +-----------+
       ^                                       ^                                          |
       |                                       |                                          |
       +----------- control plane -------------- (auth, on-demand wake) ------------------+
```

- **Firmware** talks to the backend over a single persistent WebSocket.
  Frames are pushed as binary messages; control (wake, sleep, reconfig) is
  sent as text frames.
- **Backend** terminates the camera sockets, exposes the frames to clients,
  handles auth, and stores metadata (camera registry, recent events).
- **Frontend** opens one WebSocket per visible camera tile and renders the
  live stream in the browser.

## Stack

| Layer    | Technology            | Version (checked Aug 2026) |
|----------|-----------------------|----------------------------|
| Firmware | ESP-IDF               | v5.5.3                     |
| Backend  | Go                    | latest stable              |
| Frontend | [Qwik](https://qwik.dev) | latest stable              |

> Re-verify versions with `idf.py --version`, `go version`, and your package
> manager of choice before starting a release.

## Monorepo Layout

```
.
├── firmware/      # ESP-IDF projects (one or more, e.g. leaf-node, base-station)
├── backend/       # Go server, WebSocket hub, REST API
├── frontend/      # Qwik app, live camera dashboard
├── docs/          # Architecture notes, deployment, hardware, security
├── LICENSE        # Apache 2.0
└── README.md      # this file
```

The subprojects are independent — each has its own toolchain, build, and tests.
Shared conventions live in `docs/`.

## Status

**Bootstrap.** The repository skeleton, license, and architecture intent are
in place. Firmware, backend, and frontend code are intentionally not added yet
while the architecture solidifies.

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).