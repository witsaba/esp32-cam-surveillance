import { describe, expect, test, vi } from "vitest";
import {
  CAMERA_API_BASE_URL,
  cameraStreamUrl,
  getCameraStatusLabel,
  loadCameras,
  normalizeCameras,
} from "./cameras";

describe("camera data", () => {
  test("maps the backend registry fields without synthesizing camera identity", () => {
    const [camera] = normalizeCameras({
      cameras: [
        {
          mac: "c8f09e9d5008",
          ip: "192.168.1.48",
          name: "Front gate",
          description: "North perimeter",
          fw_version: "FW-19",
          chip: "ESP32-S3",
          last_seen_at: "2026-09-03T17:00:00Z",
          status: "online",
          dropped_frames: 7,
        },
      ],
    });

    expect(camera.name).toBe("Front gate");
    expect(camera.location).toBe("North perimeter");
    expect(camera.firmware).toBe("FW-19");
    expect(camera.chip).toBe("ESP32-S3");
    expect(camera.lastSeen).toBe("2026-09-03T17:00:00Z");
    expect(camera.droppedFrames).toBe(7);
  });

  test("turns transport states into operator-facing labels", () => {
    expect(getCameraStatusLabel("online")).toBe("Live");
    expect(getCameraStatusLabel("idle")).toBe("Idle");
    expect(getCameraStatusLabel("offline")).toBe("Offline");
  });

  test("builds the localhost viewer WebSocket URL", () => {
    expect(cameraStreamUrl("c8f09e9d5008")).toBe(
      `${CAMERA_API_BASE_URL}/api/cameras/c8f09e9d5008/stream`.replace(
        "http://",
        "ws://",
      ),
    );
    expect(cameraStreamUrl("c8f09e9d5008", "https://cctv.example/"))
      .toBe("wss://cctv.example/api/cameras/c8f09e9d5008/stream");
  });

  test("loads the real registry endpoint by default", async () => {
    const fetchMock = vi.fn().mockResolvedValue(
      new Response(
        JSON.stringify({ cameras: [{ mac: "c8f09e9d5008", name: "Front gate" }] }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    );
    vi.stubGlobal("fetch", fetchMock);

    const result = await loadCameras();

    expect(fetchMock).toHaveBeenCalledWith(`${CAMERA_API_BASE_URL}/api/cameras`);
    expect(result.source).toBe("api");
    expect(result.cameras[0]?.mac).toBe("c8f09e9d5008");
    vi.unstubAllGlobals();
  });

  test("returns an explicit error state when the registry is unavailable", async () => {
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(new Error("connection refused")));

    const result = await loadCameras();

    expect(result.cameras).toEqual([]);
    expect(result.error).toBe("connection refused");
    vi.unstubAllGlobals();
  });
});
