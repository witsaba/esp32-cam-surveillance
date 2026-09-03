# CCTV control surface

## Visual direction

The frontend is an operate-first night-shift control room. It puts camera state and the selected feed ahead of marketing language or decorative dashboard metrics. The visual language is a restrained field console: ink navy surfaces, thin blue-gray rules, warm amber for primary attention, cyan for network connection, violet for idle, and red for unavailable.

## Typography

The UI uses a familiar system sans for operator-facing headings and labels. Monospaced system fallbacks are reserved for measurements, network addresses, timestamps, and camera identifiers. Headings are compact and strongly weighted; metadata stays small but high-contrast enough to scan.

## Layout

- Desktop uses a 224px utility rail and a fluid workspace.
- The workspace opens with the `Camera overview` heading and a four-cell network summary strip.
- The main stage pairs a dominant selected feed with a registry rail of camera rows.
- The feed is an authored synthetic monitoring surface with crosshair, scan field, location, status, and stream metadata.
- At 780px the rail becomes a horizontal navigation band and the stage/registry stack vertically.
- At 480px feed actions become full-width for touch use.

## Interaction and state

Camera rows use native buttons, `aria-pressed`, visible focus, and resumable Qwik event handlers. The registry filter exposes a live-only view. The display control toggles a visible mode marker. The stream action exposes a status message and is disabled for offline cameras. Online, idle, offline, loading fallback, API fallback, and empty-feed states are represented in the initial shell.

## Tokens

Primary tokens live in `frontend/src/global.css`. The surface uses `--ink` and `--panel` for the base, `--line` and `--line-strong` for structure, `--text`/`--muted`/`--quiet` for hierarchy, and `--amber`/`--cyan`/`--violet`/`--red` for semantic states.

## Future boundary

The current feed is intentionally synthetic. The route loader already accepts `PUBLIC_API_BASE_URL` and normalizes the backend `/api/cameras` response; the next implementation slice can replace the feed surface with the backend WebSocket relay at `/api/cameras/<mac>/stream` without changing the page's information architecture.
