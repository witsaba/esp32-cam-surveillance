/* capture.h — public API for the FW-11 frame-capture component.
 *
 * Single FreeRTOS task owns `esp_camera_fb_get` /
 * `esp_camera_fb_return` — the PRD § FR-2b single-owner
 * invariant. The loop body is a PURE function
 * `capture_loop_iteration()` that the FreeRTOS wrapper
 * (capture_task_entry, internal) calls inside an infinite
 * for-loop with vTaskDelay between iterations.
 *
 * Public surface (3 entry points):
 *
 *   esp_err_t capture_task_start(void)
 *       Replaces the stub at boot/stub_supervision.c:62-75.
 *       On host: records mock_supervision_record("capture")
 *       and returns ESP_OK (no FreeRTOS task).
 *       On device: spawns the FreeRTOS task that calls
 *       capture_loop_iteration() inside the for-loop.
 *
 *   uint32_t capture_fb_drops_get(void)
 *       Cross-task getter for the fb_drops counter
 *       (consumed by FW-13.6 status payload). Lock-free
 *       atomic-safe on Xtensa LX6.
 *
 *   uint32_t capture_frames_captured_get(void)
 *       Cross-task getter for the frames_captured counter.
 *
 * Test seam: the pure `capture_loop_iteration()` and the
 * `capture_queue_t` host backend are exposed here so host
 * Unity tests can drive the loop without linking FreeRTOS.
 * The `capture_queue_t` type is opaque on device (wraps
 * xQueueHandle) and a transparent slot wrapper on host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "boot_status.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- public API ---------- */
esp_err_t capture_task_start(void);
uint32_t  capture_fb_drops_get(void);
uint32_t  capture_frames_captured_get(void);

/* Reset the module-static counters + queue to their
 * fresh-boot state. Host-only test helper (mirrors the
 * mock triplet's reset() pattern). On device the counters
 * are initialised to 0 by BSS + the queue is created in
 * `capture_task_start`; reset() is a no-op there. */
void capture_counters_reset_for_test(void);

/* ---------- test seam (internal — visible for host tests) ---------- */

/* Depth of the host capture_queue_t slots — mirrors the real
 * driver xQueueCreate(2, sizeof(camera_fb_t*)) depth. */
#define MOCK_CAPTURE_QUEUE_DEPTH 2

/* camera_fb_t — we DON'T typedef it here to avoid the
 * "conflicting types" error when this header is compiled
 * alongside either mock_esp_camera.h (host) or esp_camera.h
 * (device). The public API takes a `void *` parameter; the
 * implementation casts internally. Callers that need to
 * access fields (test fixtures stack-allocating a frame)
 * include the backend header directly. */

/* capture_queue_t — opaque wrapper. On device wraps
 * xQueueHandle; on host a {static slots[2] + head/tail}. The
 * test instantiates this on the stack via zero-init + calls
 * capture_queue_send_drop_on_full() to drive it. */
typedef struct {
    void *slots[MOCK_CAPTURE_QUEUE_DEPTH];
    int   head;
    int   tail;
    int   count;
} capture_queue_t;

/* capture_counters_t — the two counters maintained by the
 * capture loop. On host tests instantiate on the stack; on
 * device these are module-static globals inside capture.c. */
typedef struct {
    uint32_t fb_drops;
    uint32_t frames_captured;
} capture_counters_t;

/* capture_queue_send_drop_on_full — push `fb` into `q`.
 * Returns true if enqueued (slot available), false if
 * dropped (queue full). The pure caller is responsible for
 * returning the buffer + bumping fb_drops on the false
 * branch — this function is purely a queue op.
 *
 * `fb` is declared as `void *` here because the real
 * `camera_fb_t` typedef differs between host (mock_esp
 * _camera.h) and device (esp_camera.h). The implementation
 * casts internally. */
bool capture_queue_send_drop_on_full(capture_queue_t *q, void *fb);

/* capture_loop_iteration — ONE iteration of the capture
 * loop: esp_camera_fb_get → optional drop-on-full →
 * frames_captured++. The FreeRTOS wrapper calls this inside
 * an infinite for-loop; host tests call it N times
 * directly. PURE (no FreeRTOS); operates on the queue +
 * counters passed in. */
void capture_loop_iteration(capture_queue_t *q, capture_counters_t *c);

#ifdef __cplusplus
}
#endif
