# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Stack

Qwik City with TypeScript and Vite, using the framework's SSR/resumability model. The frontend will consume the existing Go backend camera registry and WebSocket relay.

## Users

Inferred from the repository and request: a LAN camera operator who needs to confirm that ESP32-CAM devices are discovered and monitor their live feeds from one place.

## Product Purpose

Provide a CCTV control surface for discovering, checking, and viewing connected ESP32-CAM cameras. The first homepage should make camera availability and live monitoring obvious within seconds.

## Positioning

The system combines automatic LAN discovery with a backend-owned WebSocket ingest and NATS fan-out, so multiple viewers can consume a camera's live JPEG stream without each viewer connecting directly to the device.

## Operating Context

The operator runs the Go backend on a trusted local network. Cameras are discovered in memory through `GET /api/cameras`, and a viewer can connect to `/api/cameras/<mac>/stream`. Camera status is derived from the latest discovery observation; the backend currently distinguishes online and idle cameras.

## Capabilities and Constraints

- The homepage is the first frontend surface; additional routes are not part of this slice.
- The backend exposes camera identity, address, firmware, chip, last-seen time, status, and dropped-frame counters.
- The backend relay supports multiple viewers and uses live-only core NATS subjects; it does not persist or replay frames.
- The initial page may use clearly labeled synthetic camera states while the API/WebSocket integration is prepared for the next slice.
- Authentication, user roles, camera controls, recording, persistence, and deployment target are not yet confirmed.

## Brand Commitments

No existing frontend brand system or visual assets are present. Product naming and final brand identity remain open.

## Evidence on Hand

- `backend/README.md` documents the registry and relay endpoints.
- `backend/internal/api` and `backend/internal/relay` contain the current backend contracts.
- The user has connected an additional camera and confirmed that the backend discovers it.
- No production camera stills, logos, or marketing claims are available; demo imagery must remain synthetic or use live stream data.

## Product Principles

- Surface the real camera state before decorative content.
- Make live monitoring scannable across one or many cameras.
- Keep the first interaction useful on a slow or partially connected LAN.
- Treat connection, staleness, and stream failure as explicit states.

## Accessibility & Inclusion

Inferred baseline: keyboard-accessible controls, visible focus, semantic landmarks and headings, sufficient contrast, reduced-motion support, and status text that is not conveyed by color alone.
