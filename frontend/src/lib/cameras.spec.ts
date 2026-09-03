import { describe, expect, test } from "vitest";
import {
  demoCameras,
  getCameraStatusLabel,
  normalizeCameras,
} from "./cameras";

describe("camera data", () => {
  test("provides a useful starting wall for the homepage", () => {
    expect(demoCameras).toHaveLength(4);
    expect(demoCameras.filter((camera) => camera.status === "online")).toHaveLength(2);
  });

  test("turns transport states into operator-facing labels", () => {
    expect(getCameraStatusLabel("online")).toBe("Live");
    expect(getCameraStatusLabel("idle")).toBe("Idle");
    expect(getCameraStatusLabel("offline")).toBe("Offline");
  });

  test("normalizes the backend registry's snake_case fields", () => {
    const [camera] = normalizeCameras({
      cameras: [
        {
          mac: "c8f09e9d5008",
          name: "Front gate",
          last_seen_at: "2026-09-03T17:00:00Z",
          dropped_frames: 7,
        },
      ],
    });

    expect(camera.lastSeen).toBe("2026-09-03T17:00:00Z");
    expect(camera.droppedFrames).toBe(7);
  });
});
