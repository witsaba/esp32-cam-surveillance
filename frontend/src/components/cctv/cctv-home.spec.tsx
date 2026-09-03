import { createDOM } from "@builder.io/qwik/testing";
import { expect, test, vi } from "vitest";
import { demoCameras } from "~/lib/cameras";
import { CctvHome } from "./cctv-home";

test("renders one live camera wall tile for every discovered camera", async () => {
  vi.stubGlobal(
    "WebSocket",
    class MockWebSocket {
      binaryType = "blob";
      close() {
        this.onclose?.();
      }
      onclose?: () => void;
    },
  );
  const { screen, render } = await createDOM();
  await render(<CctvHome cameras={demoCameras} />);

  expect(screen.querySelector("h1")?.textContent).toBe("Cameras");
  expect(screen.querySelectorAll(".camera-tile")).toHaveLength(2);
  expect(screen.querySelectorAll("[data-camera-status=online]")).toHaveLength(2);
  expect(screen.querySelector("main")?.getAttribute("aria-label")).toBe(
    "CCTV cameras",
  );

  const navigation = screen.querySelector("nav");
  expect(navigation?.textContent).toContain("Cameras");
  expect(navigation?.textContent).not.toContain("Monitor");
  expect(navigation?.textContent).not.toContain("Network");
  vi.unstubAllGlobals();
});

test("shows an actionable empty state when the camera API fails", async () => {
  const { screen, render } = await createDOM();
  await render(
    <CctvHome cameras={[]} apiError="Camera API returned 503" />,
  );

  expect(screen.querySelector(".empty-feed")?.textContent).toContain(
    "Camera API unavailable",
  );
  expect(screen.querySelector('[role="alert"]')?.textContent).toContain(
    "localhost",
  );
});
