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
 * inside an infinite for-loop. FW-19 wraps it in the pure
 * time-control seam `capture_gated_iteration()` — the gate is
 * default-STOPPED and only runs while a viewer has issued
 * stream.on; the wrapper paces iterations with vTaskDelayUntil
 * at the period the seam reports.
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

/* ---------- FW-19 stream-command gate ----------
 *
 * C11 atomic {running,fps} gate (design D1): capture defaults
 * STOPPED at boot and only runs while a WS viewer has issued
 * stream.on (FW-19 closes R-25 "capture never runs without a
 * viewer"). Single-writer control ctx → single-reader capture
 * ctx; relaxed C11 atomics suffice.
 *
 * Host-build Kconfig fallbacks: the production host runner
 * carries explicit -D mirrors for every CONFIG_* symbol a
 * compiled TU references; these #ifndef guards mirror
 * sdkconfig.defaults:34-35 + main/Kconfig.projbuild:31-39
 * (and the ratified FPS_MAX=15 ceiling, ruling 2) so the
 * gated TU compiles standalone. Values are identical to the
 * runner mirrors — no redefinition conflicts.
 */
#ifndef CONFIG_FIRMWARE_STREAM_FPS
#define CONFIG_FIRMWARE_STREAM_FPS 5
#endif
#ifndef CONFIG_FIRMWARE_STREAM_FPS_MIN
#define CONFIG_FIRMWARE_STREAM_FPS_MIN 1
#endif
#ifndef CONFIG_FIRMWARE_STREAM_FPS_MAX
#define CONFIG_FIRMWARE_STREAM_FPS_MAX 15
#endif

/* Idle poll period while the gate is closed (design D1):
 * bounds start/stop latency without burning CPU. Also the
 * out->period_ms the pure gate reports while closed, so host
 * harnesses step the exact same cadence the device wrapper
 * sleeps. */
#define CAPTURE_IDLE_PERIOD_MS 100u

/* FW-19 gate lifecycle (control-task context calls these):
 *
 *   capture_run_start(fps_unclamped)
 *       Clamp via capture_fps_clamp → store applied fps →
 *       running=true (in that order so a reader never sees
 *       running=true with a stale fps).
 *   capture_run_stop()
 *       running=false. Idempotent; safe viewerless.
 *   capture_running_get()
 *       Lock-free gate read (status surfaces, handlers).
 */
void    capture_run_start(uint32_t fps_unclamped);
void    capture_run_stop(void);
bool    capture_running_get(void);
uint32_t capture_fps_clamp(long long requested);

/* Time-control seam (design D2) — ONE gated loop step as a
 * PURE function so every "≤1 simulated second" claim is
 * tick-deterministic on host:
 *
 *   in.gate_open       snapshot of s_running
 *   in.fps_applied     snapshot of s_fps (≥1 post-clamp)
 *   in.stop_requested  consumed stop word (U3 wires source)
 *
 *   out.ran            true iff capture_loop_iteration ran
 *   out.stop_latched   true iff the stop word was consumed
 *                      THIS call (caller clears the gate);
 *                      reported exactly once per stop word
 *   out.period_ms      pacing for the caller's delay:
 *                      floor(1000 / fps_applied) while open,
 *                      CAPTURE_IDLE_PERIOD_MS while closed
 *
 * Runs capture_loop_iteration iff gate_open && !stop_requested.
 * The device wrapper applies out.period_ms via vTaskDelayUntil;
 * host harnesses step discrete ticks with zero sleeps.
 */
typedef struct {
    bool     gate_open;
    uint32_t fps_applied;
    bool     stop_requested;
} capture_gate_in_t;

typedef struct {
    bool     ran;
    bool     stop_latched;
    uint32_t period_ms;
} capture_gate_out_t;

void capture_gated_iteration(capture_queue_t *q,
                             capture_counters_t *c,
                             const capture_gate_in_t *in,
                             capture_gate_out_t *out);

/* Host-only test seam: restore the module-static gate
 * (running=false, fps=CONFIG default) to fresh-boot state so
 * gate tests are order-independent (mirrors
 * capture_counters_reset_for_test). No-op on device. */
void capture_gate_reset_for_test(void);

#ifdef __cplusplus
}
#endif
