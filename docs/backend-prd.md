# PRD — Central Server Backend (Go + NATS)

> **Scope:** this document covers ONLY the central server backend that ingests camera streams, broadcasts them to viewers, discovers devices on the LAN, and owns the device registry. The frontend consumes the documented REST + WebSocket API contract; the firmware complies with the protocol contract already fixed in [`firmware-prd.md`](firmware-prd.md) § Protocol contract, which this backend implements from the server side.

---

## TL;DR

A single-process Go backend that accepts exactly **one persistent WebSocket per camera**, ingests raw JPEG frames into an **embedded core-NATS server** (`cams.<mac>.frames`, live-only, at-most-once), and fans them out to any number of browser viewers over ephemeral per-viewer WebSockets — so the chip never carries more than one connection regardless of audience. Viewership drives the camera's power state: the first viewer wakes the pipeline (`stream on`), the last one puts it back to sleep. A 60 s discovery cron sweeps the configured CIDR, validates `/whoami` responders, and upserts them into PostgreSQL (MAC primary key, IP fast-path column, full JSONB metadata). Trusted-LAN posture, no auth in v1, JPEG passthrough end-to-end — zero transcoding, zero WASM, boring components everywhere.

---

## Quick path (first 30 minutes of a new contributor)

1. Read § Goals, § Runtime target, and § FR-B-1 (camera connection lifecycle).
2. Read § Architecture (module map + data-flow diagram) and § Database model.
3. Pick up the next milestone in § Milestones. Each milestone is a self-contained PR-sized slice.
4. Cross-reference the wire format in [`firmware-prd.md`](firmware-prd.md) § Protocol contract — the backend MUST speak exactly that, byte for byte.

---

## Goals

| Goal | Measurable |
|---|---|
| The chip never saturates, no matter how many people watch | Exactly 1 ingest WebSocket per camera MAC; viewer count has zero additional connections to the device |
| Many viewers per camera | Sustain 10 concurrent viewers on one camera at QVGA @ 5 fps with no frame stalls attributable to the hub |
| Live means live | p95 frame age (camera capture → viewer WS write) < 500 ms at 5 fps |
| Late/laggy viewers never hurt healthy ones | A stalled viewer receives dropped frames (bounded channel, drop-oldest); other viewers on the same camera show zero added latency |
| The registry reflects reality | Device appears in `GET /api/cameras` within 60 s of joining the LAN (discovery cycle); status flips `online` within 2 s of camera WS connect |
| Energy goal honored end-to-end | With zero viewers, no camera is streaming: idle camera count with active streams = 0 within 5 min of last viewer leaving |
| Survives backend restarts | All cameras reconnected and `online` ≤ 60 s after backend process restart |
| Bounded resource usage | ≤ 150 MB RSS and < 1,500 goroutines at full envelope (30 cams + 300 viewers) |

## Non-goals (this PRD)

- Recording, playback, or frame persistence — core NATS only, JetStream explicitly out. A future recording PRD would add JetStream subjects without changing this architecture.
- Authentication, authorization, TLS — trusted-LAN v1, mirroring the firmware posture. A future security PRD adds tokens + `wss://`.
- Transcoding (H.264/H.265), WASM decoding, MSE/WebCodecs pipelines — JPEG passthrough only. Research verdict: browsers decode JPEG natively; WASM adds cost for zero benefit unless the pipeline abandons the ESP32's JPEG hardware encoder.
- Multi-node HA / horizontal scaling — single process, single writer. NATS makes fan-out scale *within* the box; clustering across boxes is out of scope.
- RTSP / HLS / WebRTC compatibility outputs — MediaMTX remains the escape hatch if firmware ever speaks RTSP (see § References).
- Motion detection, notifications, mobile push — backend can poll cameras on demand; intelligence lives elsewhere.
- Frontend implementation — only the API contract it consumes is fixed here.

---

## Runtime target

| Property | Value |
|---|---|
| Language | Go ≥ 1.23 |
| Process model | **Single binary** embedding nats-server v2 as a library (`server.NewServer`) |
| Database | PostgreSQL ≥ 15, accessed via [sqlc](https://sqlc.dev)-generated queries; migrations via `golang-migrate` |
| WS library | `github.com/gorilla/websocket` (revived maintainership; releases current as of 2024+) |
| Deployment | Raspberry Pi-class ARM64 host, NAS, or small VPS; bare binary + systemd is the reference shape |
| Network | Same L2/LAN as cameras; no internet dependency for core function |

Embedded-NATS ground rules (from research, mandatory): `NoSigs = true` (the app owns signal handling), options validated via `server.NewServer(opts)` (never hand-roll struct literals that skip validation), monitor/debug endpoints off by default behind a config flag, server and client modules pinned and upgraded together.

---

## Functional requirements

### FR-B-1 — Camera connection lifecycle

The backend listens on `ws://0.0.0.0:<port>/cams` (default port `8080`, configurable). This is the ONLY device-facing listener.

On camera connect the backend MUST:

1. Wait up to **5 s** for the first text frame. It MUST parse as JSON with `"type":"hello"` and a `mac` field matching `^[0-9a-f]{12}$`. Non-conforming connections are closed with close code `1008` and logged.
2. Enforce **newest-wins**: if another ingest WebSocket for the same MAC exists, the backend sends `{"type":"error","reason":"evicted_by_newer_session"}` to the OLD connection, closes it, and promotes the new one. Rationale: a camera that just rebooted must not wait out the 30 s ping-deadline on a half-open socket.
3. Upsert the device row: known MAC → `status='online'`, `last_seen_at=now()`, refresh `name`/`description`/`fw_version` from hello fields into the DB (DB is canonical per firmware FR-1a — see FR-B-6). Unknown MAC → insert with `metadata` seeded from the hello payload, `status='online'`, log `REGISTERED_VIA_HELLO`.
4. Publish a `cams.<mac>.events` frame `{"type":"camera_online","mac":"..."}` for observability.
5. On disconnect (clean or not): `status='offline'`, `last_seen_at=now()`, emit `camera_offline`.

The backend MUST NOT send binary data to cameras. Outbound traffic to cameras is text command frames only (§ FR-B-4, § Protocol contract).

### FR-B-2 — NATS ingestion (embedded, core-only)

- The ingest task publishes every binary JPEG frame to subject `cams.<mac>.frames` — raw bytes, no envelope, no base64. The MAC travels in the subject token, parsed once per connection at hello time (zero per-frame parsing).
- Control-plane observations (hello, status ticks every 30 s, `config_ok`, `error` replies from the camera) are published as JSON text to `cams.<mac>.events`.
- Core NATS semantics are load-bearing: at-most-once, no persistence, no replay. If nobody is watching a camera, its frames are published anyway while the session is active — but sessions only stay active under the viewer-refcount policy of FR-B-4, so idle publishing does not occur in practice.
- Publishing is fire-and-forget; the ingest task MUST NEVER block on publish. Backpressure belongs to subscribers (FR-B-3), not the producer.

### FR-B-3 — Viewer hub (fan-out)

The hub exposes `GET ws://<host>:<port>/api/cameras/{mac}/stream` (same port as REST).

Connection rules:

1. On upgrade, the hub checks the camera session exists (or triggers wake via FR-B-4), registers the viewer, and immediately sends one text frame `{"type":"stream_meta","mac":"...","fps":<n>}` followed by binary JPEG frames as they arrive.
2. **One writer goroutine per viewer connection** — concurrent writes to a gorilla/websocket conn are forbidden (this is the classic hub panic; the pattern is codified here).
3. Each viewer owns a **bounded channel of 8 frames** (~120 KB worst case). On overflow: drop the OLDEST frame, enqueue the newest, increment a per-viewer `dropped_frames` counter surfaced in `GET /api/cameras/{mac}`. Latest-frame semantics: a surveillance viewer wants NOW, not a replay queue.
4. The NATS-side subscription is a **single wildcard** `cams.*.frames` held for the process lifetime; frames route by subject token to the per-camera viewer set. No subscription churn per viewer.
5. The NATS callback MUST do nothing except non-blocking enqueue. **It MUST NEVER perform a synchronous viewer write.** (Research landmine: a blocked flush exceeding the server's 2 s `write_deadline` makes NATS kill the entire client connection — every camera drops simultaneously.)
6. Mandatory `SetPendingLimits(8, 131072)` per subscription-equivalent buffer and a mandatory `AsyncErrorCB` that logs `nats.ErrSlowConsumer` with the affected subject. Silent drops are forbidden — the default Go-client behavior hides them without the callback.
7. Keepalive: server ping every 10 s; missing pong for 30 s closes the viewer connection. Any client-to-server text/binary frame other than a pong → close `1008`. Viewers are read-mostly.

### FR-B-4 — On-demand streaming (viewer-refcount energy control)

The camera session manager owns the dial/close lifecycle per MAC with states `disconnected → connecting → connected_idle → streaming`:

| Transition | Trigger | Action |
|---|---|---|
| `disconnected → connecting` | First viewer requests `{mac}` (hub upgrade OR REST override) | Dial `ws://<cam>/cams` with the firmware's exponential-backoff schedule (2 s → 30 s cap, infinite retries — mirror firmware FR-4 so backend and camera never hammer each other) |
| `connecting → connected_idle` | Hello received | Send `{"cmd":"stream","on":true,"fps":CONFIG_FIRMWARE_STREAM_FPS-default 5}` |
| `connected_idle/streaming → streaming` | Viewer refcount 0 → 1 | Send `stream on` if not already streaming |
| `* → connected_idle` | Viewer refcount 1 → 0 | Grace timer 30 s (absorbs page reloads), then send `{"cmd":"stream","on":false}` |
| `connected_idle → disconnected` | Idle timer 5 min with zero viewers | Send `{"cmd":"sleep","id":...}`, await clean CLOSE, close local side. Next viewer triggers fresh dial |

Manual override: `POST /api/cameras/{mac}/stream {"on":true,"fps":12}` pins the session outside refcounting (`pinned_until` unset until an explicit `{"on":false}`). Overrides are visible in the device detail payload.

### FR-B-5 — Discovery cron

Every `discovery.interval` (default **60 s**, configurable):

1. **Self-gate:** confirm at least one non-loopback interface has a routable address in the configured CIDR; otherwise skip the cycle and log `DISCOVERY_NO_LAN`.
2. Enumerate all host addresses in the CIDR (default `192.168.1.0/24`, configurable) and probe `http://<ip>:<discovery.port default 80>/whoami` with **≤ 64 concurrent workers** and a **500 ms per-probe timeout**. A /24 sweep completes < 15 s on reference hardware.
3. Validate the response body: JSON containing `mac` matching `^[0-9a-f]{12}$` (the firmware emits colon-less lowercase hex — § firmware FR-1a `identity_mac_str`). Responders failing validation are logged as `UNIDENTIFIED_DEVICE ip=<ip>` and NOT registered. Other vendors' devices may answer `/whoami`; shape-validation is the filter.
4. Upsert: unknown MAC → insert `status='pending'`, store the ENTIRE response body in `metadata`, extract `ip`, `fw_version`, `chip` into columns. Known MAC → refresh `ip`, `metadata`, `last_whoami_at`, `fw_version`; leave `status` untouched (WS liveness owns status — scan never marks anything offline).
5. Devices whose `last_seen_at` is older than `registry.stale_after` (default 10 min) AND status ≠ `unreachable` flip to `unreachable` (a lazy sweep piggybacked on the same tick — cheap SQL, no extra probing).

### FR-B-6 — Device registry (database ownership rules)

The database is the **canonical owner of identity labels**, per firmware PRD § FR-1a: when an operator renames a camera in the backend, the device's NVS copy is stale-by-design until re-provisioning or a `config` command. Concretely:

- Hello/status frames carry the device's NVS name/description; the backend stores them in `metadata.device_reported` but the authoritative `devices.name`/`devices.description` are DB-owned and returned by the API.
- v1 provides NO label-propagation command: renaming via API changes the DB only. Push-to-device rename is a follow-up (would use the `config` text frame; noted in Open questions as resolved-deferred).

### FR-B-7 — REST API

All endpoints JSON, no auth v1, base path `/api`. Error envelope: `{"error":{"code":"<string>","message":"<string>"}}`.

| Method | Path | Purpose | Notes |
|---|---|---|---|
| `GET` | `/api/cameras` | List all devices | Items: `{mac, name, status, ip, fw_version, last_seen_at, stream_url, dropped_frames_total}` — `stream_url` is the absolute viewer WS URL: **this is the "how to access each camera individually" answer baked into the API itself** |
| `GET` | `/api/cameras/{mac}` | Detail | Adds `description`, full `metadata`, `session_state` (`disconnected/connecting/idle/streaming`), `viewers`, `pinned` |
| `POST` | `/api/cameras/{mac}/stream` | Manual stream pin/unpin | Body `{"on":bool,"fps":int?}`; `fps` clamped `[1,15]`; 409 if MAC unknown |
| `DELETE` | `/api/cameras/{mac}` | Deregister | 409 unless session state is `disconnected` (never delete a live camera) |
| `PUT` | `/api/cameras/{mac}` | Rename / describe | Body `{"name"?:≤32 chars,"description"?:≤128 chars}` — DB-only per FR-B-6 |
| `GET` | `/healthz` | Liveness | Process up |
| `GET` | `/readyz` | Readiness | NATS ready-for-connections AND `SELECT 1` on Postgres |

Unknown routes → 404 envelope. Malformed bodies → 400 naming the offending field. CORS: allow the configured frontend origin (single value, default `*` on trusted LAN, documented).

---

## Non-functional requirements

### Latency budget

| Hop | Budget |
|---|---|
| Camera capture → backend ingest parse | ≤ 100 ms p95 (network + WS framing) |
| Ingest → NATS publish → hub enqueue | ≤ 10 ms p95 (in-process, no serialization) |
| Hub dequeue → viewer WS flush | ≤ 50 ms p95 per healthy viewer |
| **End-to-end (capture → viewer write)** | **≤ 500 ms p95 at 5 fps** (≈ 2–3 frame periods) |

### Resource budgets

| Resource | Budget | Enforcement |
|---|---|---|
| Process RSS | ≤ 150 MB at 30 cams + 300 viewers | Measured in B6 soak |
| Goroutines | < 1,500 (≈ 4–5 per connection) | Runtime metric in logs every 60 s |
| File descriptors | Recommend `LimitNOFILE≥4096` (systemd unit ships with it) | Deploy note |
| Per-viewer buffer | 8 frames / 128 KB | `SetPendingLimits` (FR-B-3) |
| Discovery sweep | ≤ 64 concurrent probes; full cycle < 30 s | Worker-pool cap |

### Reliability targets

| Metric | Target |
|---|---|
| Backend restart → all cameras `online` | ≤ 60 s (camera reconnect cap 30 s dominates) |
| Registry availability during NATS outage | Registry reads unaffected (Postgres independent); streams fail visibly, recover automatically |
| DB migration safety | Forward-only migrations; `golang-migrate` versioned pairs checked in |
| Frame loss attribution | Every dropped frame increments exactly one counter (`per-viewer dropped_frames` or `ingest_dropped`) — silent loss forbidden anywhere in the pipeline |

### Security posture (v1)

- Plain `ws://` + `http://`, trusted-LAN assumption identical to the firmware PRD.
- The backend performs strict shape-validation on ALL device-supplied JSON (hello, `/whoami`) before it touches the DB — device input is never trusted as schema-stable.
- No secrets in v1 (no auth ⇒ no credentials to leak); the config file MAY contain DB DSN — file permissions documented in deployment notes.
- Viewer WS input is treated as hostile-by-default (close on unexpected frames) even though the LAN is trusted — cheap hygiene.

---

## Architecture

### Module map

```
backend/
├── cmd/server/             # main: config load, embedded NATS boot, HTTP mux, graceful shutdown
├── internal/
│   ├── conf/               # env + file config (cidr, ports, intervals, DSN)
│   ├── natsbus/            # embedded server lifecycle + conn helpers + monitor flag
│   ├── camconn/            # camera session manager (dial, hello, newest-wins, backoff, sleep)
│   ├── ingest/             # per-cam frame reader → NATS publisher (subject builder)
│   ├── hub/                # wildcard sub, router, per-viewer channels, writer goroutines
│   ├── control/            # command builder (stream/config/sleep/reboot/identify), clamps
│   ├── discovery/          # cron, CIDR walker, worker pool, /whoami validator
│   ├── registry/           # sqlc queries, status machine, upsert rules
│   └── api/                # REST handlers, envelopes, stream_url composition
├── db/migrations/          # golang-migrate pairs
├── db/queries/             # sqlc source SQL
```

### Data flow

```
                        +--------------------- single backend process ----------------------+
                        |                                                                    |
 ESP32-CAM #1 ──ws────▶ │ camconn ──▶ ingest ──publish──▶ cams.<mac>.frames ──┐             |
 ESP32-CAM #2 ──ws────▶ │ camconn ──▶ ingest ──publish─────────────────────────┼──▶ embedded │
    ...                 |                                                      │    NATS     |
 ESP32-CAM #30 ─ws────▶ │ camconn ──▶ ingest ──publish─────────────────────────┘             |
                        │                                    ▲                               |
                        │                              wildcard sub cams.*.frames            |
                        │                                    │                               |
 browser viewer ──ws───▶ hub /api/cameras/<mac>/stream ◀─────┘ (route by subject token)     |
 browser viewer ──ws───▶ hub (per-viewer chan depth 8, drop-oldest, 1 writer goroutine)     |
                        |                                                                    |
 cron (60 s) ──────────▶ discovery ──HTTP /whoami──▶ LAN devices                            |
                        |                                     │                             |
                        ├──▶ api (REST) ──▶ registry ◀────────┘ upsert                      |
                        │                       │                                            |
                        └────────────────▶ PostgreSQL (devices)                              |
                        +--------------------------------------------------------------------+
```

### Camera-session state machine

```
   viewer arrives                    hello ok                 refcount 0→1
 DISCONNECTED ──────────────▶ CONNECTING ────────▶ CONNECTED_IDLE ────────▶ STREAMING
      ▲                          │  dial fails         │  ▲                    │
      │ sleep acked /            │  (backoff 2s→30s)   │  └── grace 30s ──────┘
      │ idle 5 min               ▼                     │      refcount 1→0
      └───────────────────── (give up after              │
                              backoff continues on       STREAM_OFF sent
                              next trigger)              (state CONNECTED_IDLE)
```

---

## Database model

Single source of truth for device identity. The MAC is the primary key exactly as the wire emits it (colon-less lowercase hex, `CHAR(12)`) — no translation layer between wire format and key format.

```sql
CREATE TABLE devices (
    mac             CHAR(12)     PRIMARY KEY
                    CHECK (mac ~ '^[0-9a-f]{12}$'),
    name            TEXT         NOT NULL DEFAULT '',
    description     TEXT         NOT NULL DEFAULT '',
    ip              INET,                       -- fast-path access, refreshed by scan + hello
    status          TEXT         NOT NULL DEFAULT 'pending'
                    CHECK (status IN ('pending','online','offline','unreachable')),
    fw_version      TEXT,                       -- extracted for filtering/display
    chip            TEXT,                       -- extracted for display
    last_seen_at    TIMESTAMPTZ,                -- last WS activity (connect/frame/status)
    last_whoami_at  TIMESTAMPTZ,                -- last successful discovery probe
    metadata        JSONB        NOT NULL DEFAULT '{}'::jsonb,  -- ENTIRE /whoami body (+ hello extras)
    created_at      TIMESTAMPTZ  NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ  NOT NULL DEFAULT now()
);

CREATE INDEX idx_devices_status_last_seen ON devices (status, last_seen_at);
CREATE INDEX idx_devices_metadata_gin     ON devices USING GIN (metadata jsonb_path_ops);
```

Design rulings (each one settled, not open):

| Decision | Ruling | Why |
|---|---|---|
| MAC type | `CHAR(12)` + regex CHECK, **not** Postgres `macaddr` | The wire format is colon-less hex from `identity_mac_str()`; `macaddr` would force normalization on every write/read and silently accept formats the firmware never sends. Key = wire format, byte for byte. |
| `metadata` contents | Entire `/whoami` JSON verbatim + `device_reported` sub-object from hello/status | Requirement: full-fidelity audit of what the device said; structured columns exist only for what we filter/join on |
| Fast-path columns | `ip`, `status`, `fw_version`, `chip`, timestamps promoted out of JSONB | Requirement: IP/MAC queries must index-scan, not JSON-extract |
| GIN operator class | `jsonb_path_ops` (not default) | Smaller index, faster containment queries; we never need full-key existence checks |
| Status owner | WS lifecycle owns `online/offline`; discovery owns `pending/unreachable`; scan never demotes `online` | Two writers, disjoint states, no scan-vs-session races |
| History/audit table | None in v1 | `metadata` + timestamps cover v1 needs; an append-only `device_events` table is the natural extension when debugging demand appears |

Registry access rules: all queries through sqlc-generated typed functions; no raw SQL in service code; every mutation updates `updated_at`.

---

## Protocol contract (backend-side view)

Mirrors [`firmware-prd.md`](firmware-prd.md) § Protocol contract exactly — repeated here as the server-side obligation:

**Inbound from camera (backend MUST accept):**

| Frame | Payload | Handling |
|---|---|---|
| Text | `{"type":"hello", "mac", "name", "description", "fw", "caps":[...]}` | Connection admission (FR-B-1 step 1), registry upsert |
| Binary | Raw JPEG bytes, no envelope | Publish to `cams.<mac>.frames` untouched |
| Text | `{"type":"status", "mac", "uptime_s", "rssi_dbm", "free_heap", "fb_drops", "reconnects"}` | Refresh `last_seen_at`; merge into `metadata.device_reported`; republish on `cams.<mac>.events` |
| Text | `{"type":"config_ok"...}` / `{"type":"error", "reason", "id"}` | Log + republish on `cams.<mac>.events` |

**Outbound to camera (backend MUST send):**

| Command | Body | When |
|---|---|---|
| `stream` | `{"cmd":"stream","on":bool,"fps":int,"id":"<uuid>"}` | Refcount transitions (FR-B-4); `fps` clamped `[1,15]` client-side too |
| `sleep` | `{"cmd":"sleep","id":"<uuid>"}` | Idle timer expiry; backend then expects clean CLOSE and stops redialing until woken |
| `identify` | `{"cmd":"identify","id":"<uuid>"}` | Operator action (future UI); reply surfaces on events subject |
| `reboot` / `reset_cam` | `{"cmd":"...","id":"<uuid>"}` | Deferred to post-v1 API surface; transport support only in v1 |
| `config` | `{"cmd":"config", ...}` | Transport support only in v1 (no caller); reserved for future label-push |

Unparseable inbound text frames are logged and dropped — never crash the session, never close the connection for one bad frame.

**Viewer-facing protocol (backend ↔ browser):**

| Direction | Frame | Meaning |
|---|---|---|
| S→C text | `{"type":"stream_meta","mac":...,"fps":n}` | Immediately after upgrade, before first image |
| S→C binary | Raw JPEG | One frame per WS message, camera cadence |
| S→C text | `{"type":"camera_offline"}` | Sent before closing when the session dies mid-stream |
| C→S | pong only | Anything else → close `1008` |

---

## Milestones

| ID | Scope | Acceptance |
|---|---|---|
| **B0** | Scaffold: module layout, config loader, embedded NATS boots in-process, Postgres migrate up/down green, `go build` static binary, healthz | Binary runs on linux/arm64 + darwin/arm64; `/readyz` goes ready after NATS+PG checks; graceful shutdown drains both |
| **B1** | Registry: `devices` schema + sqlc queries + status machine + REST list/detail/delete/rename | Contract tests against real Postgres: upsert idempotent, CHECK constraints bite, indexes used (EXPLAIN) |
| **B2** | Ingest: camera WS listener, hello admission, newest-wins eviction, frame → NATS publish, status transitions | Fake camera client (test harness) streams frames; second conn evicts first; unknown-MAC hello inserts row; disconnect flips status ≤ 2 s |
| **B3** | Viewer hub: wildcard sub, router, per-viewer channels, writer-per-conn, ping/pong, drop counters | 10 fake viewers on 1 camera at 5 fps: zero panics, stalled viewer accrues `dropped_frames` while others see p95 inter-frame ≤ 220 ms; slow-consumer error path proven logged |
| **B4** | On-demand control: session manager state machine, refcount, grace + idle timers, sleep/wake, manual pin | Wake-on-viewer < 10 s (dial + hello + stream on); last-viewer + 30 s grace → stream off; idle 5 min → sleep; restart recovery ≤ 60 s |
| **B5** | Discovery: cron, CIDR walker, worker pool, `/whoami` validator, upsert/refresh, `unreachable` sweep | Integration test with stub HTTP devices: valid whoami registers, malformed skipped + logged, 254-host sweep < 15 s at 64 workers, self-gate skips when no LAN |
| **B6** | Hardening: latency instrumentation (frame-age histogram), soak 30 min at full envelope, restart-recovery proof, systemd unit, README deploy notes | Soak report: RSS ≤ 150 MB, goroutines < 1,500, p95 frame age ≤ 500 ms, zero silent-drop paths (counters reconcile) |

Each milestone ends with `go vet ./... && go test ./...` green plus a manual smoke where hardware is involved (B2 onward: real ESP32-CAM; B5: real subnet sweep).

---

## Build prerequisites

Bootstrap is three commands — no hand-maintained scaffolding beyond what tooling generates (same philosophy as firmware § Build prerequisites):

```bash
go mod init github.com/witsaba/esp32-cam-surveillance/backend
go get github.com/nats-io/nats-server/v2 github.com/nats-io/nats.go github.com/gorilla/websocket
go install github.com/sqlc-dev/sqlc/cmd/sqlc@latest
```

- `sqlc.yaml` + `db/queries/*.sql` are hand-authored (they ARE the source of truth); generated Go under `db/` is committed.
- Migrations are hand-authored pairs (`0001_init.up.sql` / `.down.sql`); everything else derives.
- Local dev needs only Docker for Postgres: `docker run -d -p 5432:5432 -e POSTGRES_PASSWORD=dev postgres:16-alpine`.

---

## Open questions

All resolved during planning (2026-08-23):

1. ✅ **Fan-out transport** — Go WS hub over NATS subjects; direct NATS-WebSocket-to-browser rejected (auth/token scoping complexity lands in frontend for zero benefit at this scale).
2. ✅ **Persistence** — live-only core NATS; JetStream named a non-goal with the migration path noted.
3. ✅ **Transcoding/WASM** — JPEG passthrough; WASM verdict: zero benefit for native-JPEG pipelines (browsers decode JPEG natively; WebCodecs covers modern codec gaps elsewhere).
4. ✅ **Camera visibility gap** — firmware serves `/whoami` ONLY on the provisioning softAP today (torn down at STA IP, FW-08.4). **Hard dependency: firmware gains a station-interface `GET /whoami` milestone (security-posture line amended) BEFORE B5 can detect ESP32 cameras.** Until it lands, B5 correctly registers only non-camera `/whoami` responders and WS-hello registration covers connected cameras.
5. ✅ **Process topology** — single binary, embedded NATS, `NoSigs=true`, monitor endpoint behind config.
6. ✅ **Scale envelope** — ≤ 30 cameras, ≤ 10 viewers each; budgets sized to that envelope in § NFRs.
7. ✅ **Label ownership** — DB canonical; no device-push rename in v1.

---

## References

Patterns below were chosen against measured evidence gathered during planning (Aug 2026). Key external findings the design depends on:

| Finding | Source | Carried forward as |
|---|---|---|
| Embedded nats-server officially supported (`NewServer`/`InProcessServer`); gotchas: options validated in constructor, `NoSigs`, coupled upgrades | docs.nats.io clients/embedding; nats-server issue #4794 | § Runtime target ground rules |
| Core NATS headroom ≈ 4 orders of magnitude above our worst case (official bench: millions msg/s vs our 150) | docs.nats.io nats bench | Live-only core NATS is sufficient; JetStream deferred confidently |
| Slow consumers: per-subscription pending limits (default 500k msgs/64 MB) DROP silently without `AsyncErrorCB`; server kills WHOLE connection on 2 s flush deadline | docs.nats.io resilient-clients/slow-consumers; write_deadline ref | FR-B-3 rules 5–6 (non-blocking callback, small limits, mandatory error cb) |
| Chrome decodes `<img>` MJPEG on the main thread; `createImageBitmap` is async but bitmaps leak without `.close()` | MotionJpegLatencyTest; EVA production writeup (2026) | Frontend guidance recorded for the frontend PRD: createImageBitmap + canvas from day one |
| MediaMTX/go2rtc cannot ingest raw JPEG over custom WebSocket (RTSP/RTMP/SRT/mpjpeg-pull/exec only) | mediamtx.org docs; go2rtc internals | Custom hub justified; MediaMTX = escape hatch if firmware ever speaks RTSP |
| gorilla/websocket revived (v1.5.3+, maintained); requires one-writer-goroutine discipline; gobwas/ws overkill at 330 conns | gorilla releases; coder/websocket docs | FR-B-3 rule 2; library choice in § Runtime target |

**Solved-problem ledger (failure modes this design refuses to re-introduce):**

| Failure mode | Where it bites | Why this PRD is immune |
|---|---|---|
| Hub blocks inside NATS callback → server write_deadline kills all camera feeds at once | Any naive fan-out loop | FR-B-3 rule 5: enqueue-only callback, writer goroutines own sockets |
| Silent slow-consumer frame loss (Go client default) | Debugging "why is viewer X frozen" | FR-B-3 rule 6 + NFR "silent loss forbidden": every drop increments a counter |
| Subscription churn per viewer (sub/unsub storms) | Viewer page reloads | FR-B-3 rule 4: one wildcard subscription for process lifetime |
| Unbounded per-viewer buffering → OOM on stalled readers | Memory budget | FR-B-3 rule 3: depth-8 channel, drop-oldest, counted |
| Camera hammering after backend restart (fixed retry interval) | Recovery storms | FR-B-4 mirrors firmware backoff schedule exactly |
| Scan marking live cameras offline (two writers racing status) | Registry flapping | DB ruling: disjoint state ownership, scan cannot demote `online` |
| Wire-format translation bugs (MAC with colons vs without) | Registry misses | DB ruling: key IS the wire format, `CHECK` enforces it |
| Hand-rolled scaffolding drifting from generator output | Build breakage | § Build prerequisites: generators own generated files |

---

## Validation status

No code exists yet — this PR precedes B0. Planned evidence per milestone is defined in § Milestones acceptance columns; B0 produces the first executable proof (embedded-NATS readiness + migration pair green). The research claims cited in § References were verified against live documentation during PRD authoring (Aug 2026) and carry their sources.
