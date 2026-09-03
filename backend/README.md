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

## NATS versions

The module pins the current stable NATS Go client and embedded server releases:

- `github.com/nats-io/nats.go v1.53.1`
- `github.com/nats-io/nats-server/v2 v2.14.6`

The next backend slice will add the device-facing WebSocket ingest path and
publish raw camera JPEG frames to `cams.<mac>.frames`.
