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
  cameras: Array<
    Partial<Camera> & {
      mac: string;
      last_seen_at?: string;
      dropped_frames?: number;
    }
  >;
  total?: number;
}

export const demoCameras: Camera[] = [
  {
    mac: "c8f09e9d5008",
    name: "Front gate",
    location: "North perimeter",
    ip: "192.168.1.48",
    firmware: "FW-19",
    chip: "ESP32-S",
    lastSeen: "Just now",
    status: "online",
    droppedFrames: 0,
    signal: "Strong",
    accent: "amber",
  },
  {
    mac: "e08cfe3091b0",
    name: "Workshop",
    location: "South building",
    ip: "192.168.1.52",
    firmware: "FW-19",
    chip: "ESP32-S",
    lastSeen: "Just now",
    status: "online",
    droppedFrames: 2,
    signal: "Good",
    accent: "blue",
  },
  {
    mac: "a4cf127b8d31",
    name: "Side door",
    location: "West passage",
    ip: "192.168.1.61",
    firmware: "FW-18",
    chip: "ESP32-S",
    lastSeen: "3 min ago",
    status: "idle",
    droppedFrames: 0,
    signal: "Fair",
    accent: "violet",
  },
  {
    mac: "78e36d4a102f",
    name: "Loading bay",
    location: "East service lane",
    ip: "192.168.1.74",
    firmware: "FW-16",
    chip: "ESP32-S",
    lastSeen: "18 min ago",
    status: "offline",
    droppedFrames: 0,
    signal: "No signal",
    accent: "red",
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
    ...demoCameras[index % demoCameras.length],
    ...camera,
    name: camera.name ?? `Camera ${index + 1}`,
    location: camera.location ?? "Unassigned",
    ip: camera.ip ?? "Unknown IP",
    firmware: camera.firmware ?? "Unknown",
    chip: camera.chip ?? "ESP32",
    lastSeen: camera.lastSeen ?? camera.last_seen_at ?? "Recently seen",
    status: camera.status ?? "online",
    droppedFrames: camera.droppedFrames ?? camera.dropped_frames ?? 0,
    signal: camera.signal ?? "Unknown",
    accent: camera.accent ?? demoCameras[index % demoCameras.length].accent,
  }));
}

export async function loadCameras(apiBaseUrl?: string): Promise<{
  cameras: Camera[];
  source: "api" | "demo";
  error?: string;
}> {
  if (!apiBaseUrl) {
    return { cameras: demoCameras, source: "demo" };
  }

  try {
    const response = await fetch(`${apiBaseUrl.replace(/\/$/, "")}/api/cameras`);
    if (!response.ok) {
      throw new Error(`Camera API returned ${response.status}`);
    }

    const payload = (await response.json()) as CameraListResponse;
    return { cameras: normalizeCameras(payload), source: "api" };
  } catch (error) {
    return {
      cameras: demoCameras,
      source: "demo",
      error: error instanceof Error ? error.message : "Camera API unavailable",
    };
  }
}
