import { component$, useSignal } from "@builder.io/qwik";
import {
  getCameraStatusLabel,
  type Camera,
  type CameraStatus,
} from "~/lib/cameras";

export interface CctvHomeProps {
  cameras: Camera[];
  source?: "api" | "demo";
  apiError?: string;
}

export const CctvHome = component$<CctvHomeProps>(
  ({ cameras, source = "demo", apiError }) => {
    const selectedMac = useSignal(
      cameras.find((camera) => camera.status === "online")?.mac ??
        cameras[0]?.mac ??
        "",
    );
    const filterOnlineOnly = useSignal(false);
    const streamRequested = useSignal(false);
    const compactMode = useSignal(false);
    const selectedCamera =
      cameras.find((camera) => camera.mac === selectedMac.value) ?? cameras[0];
    const listedCameras = filterOnlineOnly.value
      ? cameras.filter((camera) => camera.status === "online")
      : cameras;
    const onlineCount = cameras.filter(
      (camera) => camera.status === "online",
    ).length;
    const idleCount = cameras.filter(
      (camera) => camera.status === "idle",
    ).length;

    return (
      <main class="cctv-shell" aria-label="CCTV camera overview">
        <aside class="sidebar" aria-label="Primary navigation">
          <a class="brand" href="#monitor" aria-label="Cachicamas CCTV home">
            <span class="brand-mark" aria-hidden="true">
              <span />
              <span />
            </span>
            <span>
              <strong>CACHICAMAS</strong>
              <small>CCTV / LOCAL NETWORK</small>
            </span>
          </a>

          <nav class="nav-list" aria-label="CCTV sections">
            <a
              class="nav-item nav-item-active"
              href="#monitor"
              aria-current="page"
            >
              <span class="nav-icon" aria-hidden="true">
                01
              </span>
              Monitor
            </a>
            <a class="nav-item" href="#cameras">
              <span class="nav-icon" aria-hidden="true">
                02
              </span>
              Cameras
              <span class="nav-count">{cameras.length}</span>
            </a>
            <a class="nav-item" href="#network">
              <span class="nav-icon" aria-hidden="true">
                03
              </span>
              Network
            </a>
          </nav>

          <div class="sidebar-foot">
            <span class="pulse-dot" aria-hidden="true" />
            <div>
              <strong>Network active</strong>
              <small>LAN / 192.168.1.0/24</small>
            </div>
          </div>
        </aside>

        <section class="workspace" id="monitor">
          <header class="topbar">
            <div>
              <p class="section-label">OPERATIONS / MONITOR</p>
              <h1>Camera overview</h1>
            </div>
            <div class="topbar-actions">
              <span class="sync-status">
                <span class="sync-dot" aria-hidden="true" />
                Auto-refresh: 60s
              </span>
              <button
                class="icon-button"
                type="button"
                aria-label="Open display settings"
                aria-pressed={compactMode.value}
                onClick$={() => (compactMode.value = !compactMode.value)}
              >
                <span aria-hidden="true">
                  {compactMode.value ? "▦" : "•••"}
                </span>
              </button>
              <div class="operator-avatar" aria-label="Operator account">
                BA
              </div>
            </div>
          </header>

          <div class="status-strip" aria-label="Network camera summary">
            <div class="summary-primary">
              <span class="live-orb" aria-hidden="true" />
              <div>
                <strong>{onlineCount} cameras live</strong>
                <span>
                  {source === "api"
                    ? "Connected to backend registry"
                    : "Preview data · waiting for backend"}
                </span>
              </div>
            </div>
            <div class="summary-stat">
              <strong>{cameras.length}</strong>
              <span>Discovered</span>
            </div>
            <div class="summary-stat">
              <strong>{idleCount}</strong>
              <span>Idle</span>
            </div>
            <div class="summary-stat">
              <strong>0</strong>
              <span>Alerts</span>
            </div>
          </div>

          {apiError && (
            <div class="inline-notice" role="status">
              <span class="notice-mark" aria-hidden="true">
                i
              </span>
              <span>Backend preview is active. {apiError}</span>
            </div>
          )}

          <div class="content-grid">
            <section
              class="monitor-stage"
              aria-labelledby="selected-camera-heading"
            >
              <div class="panel-heading">
                <div>
                  <p class="section-label">SELECTED FEED</p>
                  <h2 id="selected-camera-heading">
                    {selectedCamera?.name ?? "No camera selected"}
                  </h2>
                </div>
                {selectedCamera && (
                  <StatusBadge status={selectedCamera.status} />
                )}
              </div>
              {selectedCamera ? (
                <CameraFeed camera={selectedCamera} />
              ) : (
                <EmptyFeed />
              )}
              {selectedCamera && (
                <div class="feed-footer">
                  <div class="feed-meta">
                    <span>
                      <strong>IP</strong>
                      {selectedCamera.ip}
                    </span>
                    <span>
                      <strong>MAC</strong>
                      {selectedCamera.mac}
                    </span>
                    <span>
                      <strong>Signal</strong>
                      {selectedCamera.signal}
                    </span>
                  </div>
                  <button
                    class={{
                      "secondary-button": true,
                      "secondary-button-disabled":
                        selectedCamera.status === "offline",
                    }}
                    type="button"
                    disabled={selectedCamera.status === "offline"}
                    onClick$={() => (streamRequested.value = true)}
                  >
                    <span aria-hidden="true">
                      {selectedCamera.status === "offline" ? "—" : "↗"}
                    </span>
                    {selectedCamera.status === "offline"
                      ? "Stream unavailable"
                      : "Open stream"}
                  </button>
                </div>
              )}
              {streamRequested.value && (
                <p class="stream-state" role="status">
                  Stream slot ready for {selectedCamera?.name ?? "this camera"}.
                </p>
              )}
            </section>

            <aside
              class="camera-rail"
              id="cameras"
              aria-labelledby="camera-list-heading"
            >
              <div class="panel-heading rail-heading">
                <div>
                  <p class="section-label">REGISTRY</p>
                  <h2 id="camera-list-heading">All cameras</h2>
                </div>
                <button
                  class="text-button"
                  type="button"
                  aria-pressed={filterOnlineOnly.value}
                  onClick$={() =>
                    (filterOnlineOnly.value = !filterOnlineOnly.value)
                  }
                >
                  {filterOnlineOnly.value ? "Live only" : "Filter"}
                </button>
              </div>
              <div class="camera-list">
                {listedCameras.map((camera) => (
                  <button
                    key={camera.mac}
                    class={{
                      "camera-row": true,
                      "camera-row-selected": camera.mac === selectedMac.value,
                    }}
                    type="button"
                    aria-pressed={camera.mac === selectedMac.value}
                    data-camera-card
                    onClick$={() => (selectedMac.value = camera.mac)}
                  >
                    <CameraThumbnail camera={camera} />
                    <span class="camera-row-copy">
                      <span class="camera-row-title">{camera.name}</span>
                      <span class="camera-row-location">{camera.location}</span>
                      <span
                        class="camera-row-status"
                        data-camera-status={camera.status}
                      >
                        <span class="status-dot" aria-hidden="true" />
                        {getCameraStatusLabel(camera.status)} ·{" "}
                        {camera.lastSeen}
                      </span>
                    </span>
                    <span class="row-chevron" aria-hidden="true">
                      →
                    </span>
                  </button>
                ))}
              </div>
              <div class="rail-footer">
                <span class="mini-radar" aria-hidden="true" />
                <span>
                  Discovery sweep
                  <br />
                  <strong>Next check in 42s</strong>
                </span>
              </div>
            </aside>
          </div>

          <footer class="page-footer" id="network">
            <span>ESP32-CAM SURVEILLANCE / CONTROL SURFACE</span>
            <span>All times local · Last registry sync just now</span>
          </footer>
        </section>
      </main>
    );
  },
);

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

export const CameraFeed = component$<{ camera: Camera }>(({ camera }) => (
  <div
    class={{ "camera-feed": true, [`camera-feed-${camera.accent}`]: true }}
    aria-label={`${camera.name} camera feed`}
  >
    <div class="feed-grid" aria-hidden="true" />
    <div class="feed-noise" aria-hidden="true" />
    <div class="feed-topline">
      <span>CAM / {camera.mac.slice(-4).toUpperCase()}</span>
      <span>
        {camera.status === "online" ? "JPEG · 5 FPS" : "NO ACTIVE STREAM"}
      </span>
    </div>
    <div class="feed-center-mark" aria-hidden="true">
      <span />
      <span />
    </div>
    <div class="feed-overlay">
      <span
        class={{
          "feed-live": camera.status === "online",
          "feed-state": true,
        }}
      >
        <span class="status-dot" aria-hidden="true" />
        {getCameraStatusLabel(camera.status).toUpperCase()}
      </span>
      <span class="feed-time">12:42:08</span>
    </div>
    <div class="feed-caption">
      <strong>{camera.location}</strong>
      <span>
        {camera.status === "online"
          ? "Stream preview / synthetic frame"
          : "No active stream / registry state"}
      </span>
    </div>
  </div>
));

export const CameraThumbnail = component$<{ camera: Camera }>(({ camera }) => (
  <span
    class={{
      "camera-thumbnail": true,
      [`camera-thumbnail-${camera.accent}`]: true,
    }}
    aria-hidden="true"
  >
    <span class="thumbnail-grid" />
    <span class="thumbnail-cross" />
    <span class="thumbnail-code">{camera.mac.slice(-2).toUpperCase()}</span>
  </span>
));

export const EmptyFeed = component$(() => (
  <div class="empty-feed" role="status">
    <span class="empty-feed-icon" aria-hidden="true">
      —
    </span>
    <strong>No camera feed available</strong>
    <span>Select an online camera from the registry.</span>
  </div>
));
