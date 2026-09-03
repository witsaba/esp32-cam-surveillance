import { createDOM } from "@builder.io/qwik/testing";
import { expect, test } from "vitest";
import { demoCameras } from "~/lib/cameras";
import { CctvHome } from "./cctv-home";

test("renders the CCTV monitor wall with clear camera states", async () => {
  const { screen, render, userEvent } = await createDOM();
  await render(<CctvHome cameras={demoCameras} />);

  expect(screen.querySelector("h1")?.textContent).toContain("Camera overview");
  expect(screen.querySelectorAll("[data-camera-card]")).toHaveLength(4);
  expect(screen.querySelector('[data-camera-status="online"]')?.textContent).toContain("Live");
  expect(screen.querySelector('[data-camera-status="idle"]')?.textContent).toContain("Idle");
  expect(screen.querySelector("main")?.getAttribute("aria-label")).toBe("CCTV camera overview");

  await userEvent(".text-button", "click");
  expect(screen.querySelectorAll("[data-camera-card]")).toHaveLength(2);
  expect(screen.querySelector(".text-button")?.textContent).toContain("Live only");

  await userEvent(".secondary-button", "click");
  expect(screen.querySelector('[role="status"]')?.textContent).toContain("Stream slot ready");
});
