# CCTV control surface

## Visual direction

The frontend is an operate-first night-shift control room. It puts camera state and the selected feed ahead of marketing language or decorative dashboard metrics. The visual language is a restrained field console: ink navy surfaces, thin blue-gray rules, warm amber for primary attention, cyan for network connection, violet for idle, and red for unavailable.

## Typography

The UI uses a familiar system sans for operator-facing headings and labels. Monospaced system fallbacks are reserved for measurements, network addresses, timestamps, and camera identifiers. Headings are compact and strongly weighted; metadata stays small but high-contrast enough to scan.

## Layout

- Desktop uses a 224px utility rail with one Cameras destination and a fluid workspace.
- The workspace opens with the `Cameras` heading and a four-cell camera summary strip.
- The main surface is a live camera wall with one relay-backed tile per discovered camera.
- Each tile pairs the real JPEG feed with location, status, stream state, IP, MAC, and firmware metadata.
- At 780px the rail becomes a horizontal navigation band and the camera wall stacks into one column at 700px.
- On small screens camera metadata wraps while the live image keeps its aspect ratio.

## Interaction and state

The route loads the real registry from localhost and each visible tile opens its own read-only viewer WebSocket. Binary JPEG frames become object URLs that are revoked when replaced or unmounted. Connecting, live, reconnecting, unavailable, API-error, and empty-registry states are explicit and communicated with text as well as color.

## Tokens

Primary tokens live in `frontend/src/global.css`. The surface uses `--ink` and `--panel` for the base, `--line` and `--line-strong` for structure, `--text`/`--muted`/`--quiet` for hierarchy, and `--amber`/`--cyan`/`--violet`/`--red` for semantic states.

## Future boundary

The live source defaults to `http://localhost:8080` and can be overridden with `PUBLIC_API_BASE_URL`. The browser connects to `/api/cameras/<mac>/stream`; the backend sends `stream_meta` followed by binary JPEG frames. Recording, authentication, controls, and history remain outside this surface.
