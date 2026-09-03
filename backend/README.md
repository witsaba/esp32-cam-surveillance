# Backend

The backend discovers ESP32-CAM devices, opens one WebSocket ingest connection
per camera when a viewer asks for it, publishes raw JPEGs through embedded
core-NATS, and fans those frames out to any number of backend viewers.

## Run

```sh
make get-deps
make test
make build
NATS_HOST=127.0.0.1 NATS_PORT=4222 ./bin/cam-surveillance
```

The process stays up until `SIGINT` or `SIGTERM`. `NATS_PORT` defaults to
`4222`; `NATS_HOST` defaults to `127.0.0.1`.

The camera registry is available at `GET /api/cameras` on the HTTP listener.
The listener defaults to `127.0.0.1:8080` and can be changed with `HTTP_HOST`
and `HTTP_PORT`. The endpoint reads the same in-memory registry populated by
LAN discovery, so a successfully discovered camera appears immediately. Each
item includes its identity, address, firmware, `last_seen_at`, and a derived
status: `online` through two minutes after its last observation, then `idle`.
Pass `?status=online` or `?status=idle` to filter the list. The default
localhost bind keeps this unauthenticated metadata endpoint local; set
`HTTP_HOST` explicitly when exposing it to a trusted LAN.

## Relay viewer

After discovery, a viewer connects to:

```text
ws://127.0.0.1:8080/api/cameras/<mac>/stream
```

The first viewer starts the camera's stream command. Additional viewers reuse
the same camera WebSocket and receive the same raw JPEG feed through NATS.
Each viewer has an eight-frame latest-frame queue; dropped frames are counted
in `dropped_frames` in the camera list response.

The stdlib-only verifier asks for all online cameras, opens one viewer per
camera, and saves a few JPEGs:

```sh
python3 backend/scripts/verify_streams.py --frames 5
```

Relay settings are `CAMERA_WS_PORT` (default `80`), `CAMERA_WS_PATH`
(default `/cams`), and `STREAM_FPS` (default `5`, clamped to `1..15`).

On startup the backend launches a background LAN discovery sweep. It probes
every usable host in the configured IPv4 CIDR with `GET /whoami`, using at
most 64 concurrent probes and a 500 ms timeout. The first sweep runs
immediately and repeats every minute. With no `DISCOVERY_CIDR`, the scanner
derives ranges from active, non-loopback IPv4 interfaces. Successful devices
are stored in an in-memory registry keyed by their 12-character lowercase MAC;
repeated observations refresh the address, metadata, and last-seen timestamp.
The server logs only successful camera discoveries (`DISCOVERY_CAMERA`); failed
or malformed probes are silently skipped.

Discovery can be tuned with `DISCOVERY_CIDR`, `DISCOVERY_INTERVAL`,
`DISCOVERY_TIMEOUT`, `DISCOVERY_PORT`, and `DISCOVERY_WORKERS` (workers are
bounded to 64). Discovery never marks a camera offline; that state belongs to
the future live connection manager.

## NATS versions

The module pins the current stable NATS Go client and embedded server releases:

- `github.com/nats-io/nats.go v1.53.1`
- `github.com/nats-io/nats-server/v2 v2.14.6`

The relay uses `cams.<mac>.frames` for raw JPEGs and `cams.<mac>.events` for
camera text frames. Core NATS is live-only: no frame persistence or replay is
attempted.
