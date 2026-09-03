# Backend

The backend currently provides the smallest runnable foundation for the
camera relay: a Go module that starts an embedded core-NATS server and keeps an
in-process NATS client connection available for the upcoming camera ingest and
viewer hub.

## Run

```sh
make get-deps
make test
make build
NATS_HOST=127.0.0.1 NATS_PORT=4222 ./bin/cam-surveillance
```

The process stays up until `SIGINT` or `SIGTERM`. `NATS_PORT` defaults to
`4222`; `NATS_HOST` defaults to `127.0.0.1`.

On startup the backend launches a background LAN discovery sweep. It probes
every usable host in the configured IPv4 CIDR with `GET /whoami`, using at
most 64 concurrent probes and a 500 ms timeout. The first sweep runs
immediately and repeats every minute. With no `DISCOVERY_CIDR`, the scanner
derives ranges from active, non-loopback IPv4 interfaces. Successful devices
are stored in an in-memory registry keyed by their 12-character lowercase MAC;
repeated observations refresh the address, metadata, and last-seen timestamp.

Discovery can be tuned with `DISCOVERY_CIDR`, `DISCOVERY_INTERVAL`,
`DISCOVERY_TIMEOUT`, `DISCOVERY_PORT`, and `DISCOVERY_WORKERS` (workers are
bounded to 64). Discovery never marks a camera offline; that state belongs to
the future live connection manager.

## NATS versions

The module pins the current stable NATS Go client and embedded server releases:

- `github.com/nats-io/nats.go v1.53.1`
- `github.com/nats-io/nats-server/v2 v2.14.6`

The next backend slice will add the device-facing WebSocket ingest path and
publish raw camera JPEG frames to `cams.<mac>.frames`.
