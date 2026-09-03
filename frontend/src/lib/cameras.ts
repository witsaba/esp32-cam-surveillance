export const CAMERA_API_BASE_URL = "http://localhost:8080";

export type CameraStatus = "online" | "idle" | "offline";

export interface Camera {
  mac: string;
  name: string;
  location: string;
  ip: string;
  firmware: string;
  chip: string;
  lastSeen: string;
  status: CameraStatus;
  droppedFrames: number;
  signal: string;
  accent: "amber" | "blue" | "violet" | "red";
}

export interface CameraListResponse {
  cameras: Array<{
    mac: string;
    ip?: string;
    name?: string;
    description?: string;
    firmware?: string;
    fw_version?: string;
    chip?: string;
    lastSeen?: string;
    last_seen_at?: string;
    status?: CameraStatus;
    droppedFrames?: number;
    dropped_frames?: number;
  }>;
  total?: number;
}

const accents: Camera["accent"][] = ["amber", "blue", "violet", "red"];

// Test fixture only. The route never falls back to this data when the API is unavailable.
export const demoCameras: Camera[] = [
  {
    mac: "c8f09e9d5008",
    name: "Front gate",
    location: "North perimeter",
    ip: "192.168.1.48",
    firmware: "FW-19",
    chip: "ESP32",
    lastSeen: "Just now",
    status: "online",
    droppedFrames: 0,
    signal: "LAN",
    accent: "amber",
  },
  {
    mac: "e08cfe3091b0",
    name: "Workshop",
    location: "South building",
    ip: "192.168.1.52",
    firmware: "FW-19",
    chip: "ESP32",
    lastSeen: "Just now",
    status: "online",
    droppedFrames: 2,
    signal: "LAN",
    accent: "blue",
  },
];

export function getCameraStatusLabel(status: CameraStatus): string {
  switch (status) {
    case "online":
      return "Live";
    case "idle":
      return "Idle";
    case "offline":
      return "Offline";
  }
}

export function normalizeCameras(response: CameraListResponse): Camera[] {
  return response.cameras.map((camera, index) => ({
    mac: camera.mac,
    name: camera.name?.trim() || `Camera ${index + 1}`,
    location: camera.description?.trim() || "Unassigned",
    ip: camera.ip || "Unknown IP",
    firmware: camera.firmware || camera.fw_version || "Unknown",
    chip: camera.chip || "ESP32",
    lastSeen: camera.lastSeen || camera.last_seen_at || "Recently seen",
    status: camera.status || "online",
    droppedFrames: camera.droppedFrames ?? camera.dropped_frames ?? 0,
    signal: "LAN",
    accent: accents[index % accents.length],
  }));
}

export function cameraStreamUrl(
  mac: string,
  apiBaseUrl = CAMERA_API_BASE_URL,
): string {
  const base = apiBaseUrl.replace(/\/$/, "");
  const websocketBase = base.replace(/^http/, "ws");
  return `${websocketBase}/api/cameras/${encodeURIComponent(mac)}/stream`;
}

export async function loadCameras(
  apiBaseUrl = CAMERA_API_BASE_URL,
): Promise<{
  cameras: Camera[];
  source: "api";
  error?: string;
}> {
  try {
    const response = await fetch(`${apiBaseUrl.replace(/\/$/, "")}/api/cameras`);
    if (!response.ok) {
      throw new Error(`Camera API returned ${response.status}`);
    }

    const payload = (await response.json()) as CameraListResponse;
    return { cameras: normalizeCameras(payload), source: "api" };
  } catch (error) {
    return {
      cameras: [],
      source: "api",
      error: error instanceof Error ? error.message : "Camera API unavailable",
    };
  }
}
