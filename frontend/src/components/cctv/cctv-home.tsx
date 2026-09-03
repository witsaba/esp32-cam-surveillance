import { component$, useSignal, useVisibleTask$ } from "@builder.io/qwik";
import {
  CAMERA_API_BASE_URL,
  cameraStreamUrl,
  getCameraStatusLabel,
  type Camera,
  type CameraStatus,
} from "~/lib/cameras";

export interface CctvHomeProps {
  cameras: Camera[];
  apiBaseUrl?: string;
  apiError?: string;
}

export const CctvHome = component$<CctvHomeProps>(
  ({ cameras, apiBaseUrl = CAMERA_API_BASE_URL, apiError }) => {
    const onlineCount = cameras.filter(
      (camera) => camera.status === "online",
    ).length;
    const droppedFrameCount = cameras.reduce(
      (total, camera) => total + camera.droppedFrames,
      0,
    );

    return (
      <main class="cctv-shell" aria-label="CCTV cameras">
        <aside class="sidebar" aria-label="Primary navigation">
          <a class="brand" href="#cameras" aria-label="Cachicamas cameras home">
            <span class="brand-mark" aria-hidden="true">
              <span />
              <span />
            </span>
            <span>
              <strong>CACHICAMAS</strong>
              <small>CCTV / LOCAL CAMERAS</small>
            </span>
          </a>

          <nav class="nav-list" aria-label="Camera sections">
            <a
              class="nav-item nav-item-active"
              href="#cameras"
              aria-current="page"
            >
              <span class="nav-icon" aria-hidden="true">
                ◉
              </span>
              Cameras
              <span class="nav-count">{cameras.length}</span>
            </a>
          </nav>

          <div class="sidebar-foot">
            <span class="pulse-dot" aria-hidden="true" />
            <div>
              <strong>Local API</strong>
              <small>{apiBaseUrl.replace(/^https?:\/\//, "")}</small>
            </div>
          </div>
        </aside>

        <section class="workspace" id="cameras">
          <header class="topbar">
            <div>
              <p class="section-label">CAMERAS / LIVE WALL</p>
              <h1>Cameras</h1>
            </div>
            <div class="topbar-actions">
              <span class="sync-status">
                <span class="sync-dot" aria-hidden="true" />
                Source: localhost:8080
              </span>
              <div class="operator-avatar" aria-label="Operator account">
                BA
              </div>
            </div>
          </header>

          <div class="status-strip" aria-label="Camera summary">
            <div class="summary-primary">
              <span class="live-orb" aria-hidden="true" />
              <div>
                <strong>
                  {onlineCount} {onlineCount === 1 ? "camera" : "cameras"} live
                </strong>
                <span>
                  {apiError
                    ? "Backend connection needs attention"
                    : "Live from local registry"}
                </span>
              </div>
            </div>
            <div class="summary-stat">
              <strong>{cameras.length}</strong>
              <span>Discovered</span>
            </div>
            <div class="summary-stat">
              <strong>{cameras.length - onlineCount}</strong>
              <span>Idle</span>
            </div>
            <div class="summary-stat">
              <strong>{droppedFrameCount}</strong>
              <span>Dropped frames</span>
            </div>
          </div>

          {apiError && (
            <div class="inline-notice" role="alert">
              <span class="notice-mark" aria-hidden="true">
                !
              </span>
              <span>
                Cannot load cameras from localhost. Start the backend at{" "}
                <code>{apiBaseUrl}</code> and refresh this page.
              </span>
            </div>
          )}

          <section class="camera-wall" aria-labelledby="camera-wall-heading">
            <div class="panel-heading">
              <div>
                <h2 id="camera-wall-heading">All cameras</h2>
                <p class="panel-subtitle">
                  Each tile is connected directly to the backend viewer relay.
                </p>
              </div>
              <span class="status-badge status-badge-online">
                <span class="status-dot" aria-hidden="true" />
                {cameras.length ? "AUTO-CONNECT" : "WAITING"}
              </span>
            </div>

            {cameras.length ? (
              <div class="camera-grid">
                {cameras.map((camera) => (
                  <LiveCameraTile
                    key={camera.mac}
                    camera={camera}
                    apiBaseUrl={apiBaseUrl}
                  />
                ))}
              </div>
            ) : (
              <EmptyFeed hasError={Boolean(apiError)} />
            )}
          </section>

          <footer class="page-footer">
            <span>ESP32-CAM SURVEILLANCE / CAMERAS</span>
            <span>Frames are relayed live · no recording</span>
          </footer>
        </section>
      </main>
    );
  },
);

type StreamState = "connecting" | "live" | "error" | "closed";

export const LiveCameraTile = component$<{
  camera: Camera;
  apiBaseUrl: string;
}>(({ camera, apiBaseUrl }) => {
  const frameUrl = useSignal("");
  const streamState = useSignal<StreamState>(
    camera.status === "offline" ? "closed" : "connecting",
  );
  const streamError = useSignal("");
  const streamFps = useSignal<number | undefined>(undefined);

  // The viewer protocol is intentionally client-only: WebSocket and object URL
  // APIs do not exist during Qwik SSR.
  // eslint-disable-next-line qwik/no-use-visible-task
  useVisibleTask$(({ cleanup }) => {
    if (camera.status === "offline") {
      return;
    }

    let socket: WebSocket | undefined;
    let retryTimer: ReturnType<typeof setTimeout> | undefined;
    let disposed = false;

    const connect = () => {
      if (disposed) return;

      streamState.value = "connecting";
      socket = new WebSocket(cameraStreamUrl(camera.mac, apiBaseUrl));
      socket.binaryType = "blob";
      socket.onopen = () => {
        streamError.value = "";
      };
      socket.onmessage = (event) => {
        if (typeof event.data === "string") {
          try {
            const message = JSON.parse(event.data) as {
              type?: string;
              fps?: number;
            };
            if (message.type === "stream_meta") {
              streamFps.value = message.fps;
            }
          } catch {
            streamError.value = "Invalid stream metadata";
          }
          return;
        }

        if (
          !(event.data instanceof Blob) &&
          !(event.data instanceof ArrayBuffer)
        ) {
          return;
        }

        const nextFrame =
          event.data instanceof Blob
            ? event.data
            : new Blob([event.data], { type: "image/jpeg" });
        const nextUrl = URL.createObjectURL(nextFrame);
        const previousUrl = frameUrl.value;
        frameUrl.value = nextUrl;
        if (previousUrl) URL.revokeObjectURL(previousUrl);
        streamState.value = "live";
      };
      socket.onerror = () => {
        streamState.value = "error";
        streamError.value = "Stream unavailable · retrying";
      };
      socket.onclose = () => {
        if (disposed) return;
        streamState.value = "error";
        streamError.value = "Connection closed · retrying";
        retryTimer = globalThis.setTimeout(connect, 3000);
      };
    };

    connect();
    cleanup(() => {
      disposed = true;
      if (retryTimer) globalThis.clearTimeout(retryTimer);
      socket?.close();
      if (frameUrl.value) URL.revokeObjectURL(frameUrl.value);
    });
  });

  const stateLabel =
    streamState.value === "live"
      ? `Live${streamFps.value ? ` · ${streamFps.value} FPS` : ""}`
      : streamState.value === "connecting"
        ? "Connecting"
        : streamState.value === "closed"
          ? "Unavailable"
          : "Reconnecting";

  return (
    <article class="camera-tile">
      <header class="camera-tile-header">
        <div>
          <h3>{camera.name}</h3>
          <p>{camera.location}</p>
        </div>
        <StatusBadge status={camera.status} />
      </header>
      <div
        class={{
          "camera-stream": true,
          [`camera-stream-${camera.accent}`]: true,
          "camera-stream-has-frame": Boolean(frameUrl.value),
        }}
        aria-live="polite"
      >
        {frameUrl.value ? (
          <img
            src={frameUrl.value}
            alt={`Live feed from ${camera.name}`}
            width="640"
            height="400"
          />
        ) : (
          <div class="stream-placeholder">
            <span class="stream-crosshair" aria-hidden="true">
              <span />
            </span>
            <strong>{stateLabel}</strong>
            <span>
              {streamError.value ||
                (camera.status === "idle"
                  ? "Camera is idle in the registry"
                  : "Waiting for the first JPEG frame")}
            </span>
          </div>
        )}
        <div class="stream-overlay">
          <span class={`stream-state stream-state-${streamState.value}`}>
            <span class="status-dot" aria-hidden="true" />
            {stateLabel}
          </span>
          <span>CAM / {camera.mac.slice(-4).toUpperCase()}</span>
        </div>
      </div>
      <footer class="camera-tile-footer">
        <span>
          <strong>IP</strong>
          {camera.ip}
        </span>
        <span>
          <strong>MAC</strong>
          {camera.mac}
        </span>
        <span>
          <strong>FW</strong>
          {camera.firmware}
        </span>
      </footer>
    </article>
  );
});

export const StatusBadge = component$<{ status: CameraStatus }>(
  ({ status }) => (
    <span
      class={{ "status-badge": true, [`status-badge-${status}`]: true }}
      data-camera-status={status}
    >
      <span class="status-dot" aria-hidden="true" />
      {getCameraStatusLabel(status)}
    </span>
  ),
);

export const EmptyFeed = component$<{ hasError?: boolean }>(({ hasError }) => (
  <div class="empty-feed" role="status">
    <span class="empty-feed-icon" aria-hidden="true">
      {hasError ? "!" : "—"}
    </span>
    <strong>
      {hasError ? "Camera API unavailable" : "No cameras discovered"}
    </strong>
    <span>
      {hasError
        ? "Start the local backend and refresh to load its registry."
        : "The backend registry is empty. Waiting for a discovery sweep."}
    </span>
  </div>
));
