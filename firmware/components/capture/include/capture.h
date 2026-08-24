/* capture.h — public API for the FW-11 frame-capture component.
 *
 * Frame-buffer ownership (amended in FW-15, REQ-ST-005):
 * the PRODUCER owns the fb from `esp_camera_fb_get()` up to a
 * successful enqueue; ownership TRANSFERS TO THE CONSUMER at
 * receive (`capture_queue_receive_timeout`). The producer still
 * calls `esp_camera_fb_return` for frames it drops on a full
 * queue. Before FW-15 this file claimed only the capture TU may
 * call fb_return — that wording is superseded by the
 * transfer-at-receive rule, which is the only leak-free contract
 * once a consumer exists.
 *
 * The loop body is a PURE function `capture_loop_iteration()`
 * that the FreeRTOS wrapper (capture_task_entry, internal) calls
 * inside an infinite for-loop with vTaskDelay between iterations.
 *
 * Public surface:
 *
 *   esp_err_t capture_task_start(void)
 *       Starts the capture loop. On host: records
 *       mock_supervision_record("capture") + arms the queue sync
 *       hooks (below). On device: spawns the FreeRTOS task.
 *
 *   uint32_t capture_fb_drops_get(void)
 *   uint32_t capture_frames_captured_get(void)
 *       Cross-task getters (FW-13.6 status payload). Lock-free
 *       atomic-safe on Xtensa LX6.
 *
 *   bool capture_queue_receive_timeout(void **out, uint32_t ms)
 *       FW-15 consumer seam (REQ-ST-006): pops the head of the
 *       module-static capture queue, waiting at most `ms`
 *       milliseconds. Returns true + writes *out when an item was
 *       received; false on timeout. NEVER blocks forever.
 *       Operates on the module-static g_capture_queue (the same
 *       instance the device capture task fills).
 *
 * Test seam: the pure `capture_loop_iteration()`, the
 * `capture_queue_t` host backend, and `capture_queue_for_test()`
 * are exposed here so host Unity tests can drive producer +
 * consumer without linking FreeRTOS. The `capture_queue_t` type
 * is opaque on device (wraps xQueueHandle) and a transparent slot
 * wrapper on host.
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
 * capture_queue_send_drop_on_full() to drive it.
 *
 * sync_mtx / sync_cv (FW-15, design D2): optional cross-task
 * sync hooks for the SPSC producer→consumer handoff.
 *   - NULL (default, stack-instantiated test queues): the
 *     queue ops stay lock-free and pure.
 *   - Non-NULL: set on the module-static instance by
 *     capture_task_start(). Host points them at pthread
 *     mutex + condvar; device at FreeRTOS mutex + counting
 *     semaphore. send_drop_on_full locks/signals only when
 *     the hooks are armed; receive_timeout waits on them
 *     with a bounded deadline. */
typedef struct {
    void *slots[MOCK_CAPTURE_QUEUE_DEPTH];
    int   head;
    int   tail;
    int   count;
    void *sync_mtx;
    void *sync_cv;
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
 * When `q` carries non-NULL sync hooks (the cross-task
 * module-static instance) the push takes the mutex and
 * signals the condvar/semaphore on success so a blocked
 * consumer wakes. Stack queues with NULL hooks stay
 * lock-free and pure.
 *
 * `fb` is declared as `void *` here because the real
 * `camera_fb_t` typedef differs between host (mock_esp
 * _camera.h) and device (esp_camera.h). The implementation
 * casts internally. */
bool capture_queue_send_drop_on_full(capture_queue_t *q, void *fb);

/* capture_queue_receive_timeout — FW-15 (REQ-ST-006): pop the
 * head of the MODULE-STATIC capture queue, waiting at most
 * `timeout_ms`. True + *out on success; false on timeout or
 * invalid args. Host: pthread_cond_timedwait against
 * CLOCK_MONOTONIC on the static ring. Device: thin wrapper
 * over the FreeRTOS mutex + counting semaphore installed at
 * capture_task_start. Never blocks forever. */
bool capture_queue_receive_timeout(void **out, uint32_t timeout_ms);

/* Host-only test seam: pointer to the module-static queue, so
 * host tests can play producer into the SAME ring that
 * capture_queue_receive_timeout consumes (mirrors the
 * capture_counters_reset_for_test reset-seam pattern).
 * Device builds must not call this. */
capture_queue_t *capture_queue_for_test(void);

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
