/* stream.h — public API for the FW-15/FW-16 stream component.
 *
 * Consumes the FW-11 depth-2 capture queue and ships each camera
 * frame as ONE complete binary WebSocket message through the
 * viewer sink (FW-16 server mode: httpd_ws_send_frame_async with
 * hd+fd captured at /cams handshake).
 *
 * Ownership (REQ-ST-005, amended capture.h contract): the stream
 * task owns the frame buffer from receive until AFTER the send
 * attempt — esp_camera_fb_return fires on success AND failure.
 *
 * Split per design: pure planner (stream_fragment.{c.h}, retained
 * as a diagnostic — feeds the greppable parts= log metric) + thin
 * sender (stream_sender.c) + FreeRTOS task loop (stream.c),
 * mirroring the capture component's shape.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot_status.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- task lifecycle ---------- */

/* Replaces the deleted stub body at boot/stub_supervision.c
 * (T-13-I pattern; linker resolves the strong symbol). On host:
 * records mock_supervision_record("stream") and honours
 * mock_init_returns_get(BOOT_STEP_SUPERVISION_STREAM). On device:
 * spawns the "stream" FreeRTOS task (stack/prio mirror capture's).
 */
esp_err_t stream_task_start(void);

/* ---------- cross-task counters ---------- */

/* Stream-owned counters. Deliberately SEPARATE from
 * capture_fb_drops_get() (producer-only): mixing producer and
 * consumer drops would corrupt the FW-13.6 status semantics.
 * uint32_t reads are atomic on Xtensa LX6. */
uint32_t stream_frames_sent_get(void);
uint32_t stream_frames_dropped_get(void);

/* Host-only test seam: reset the module-static counters +
 * loop state between tests. No-op on device. */
void stream_counters_reset_for_test(void);

/* ---------- test surface ---------- */

/* One iteration of the consume→send→return cycle:
 *   fb = capture_queue_receive_timeout(STREAM_RECEIVE_TIMEOUT_MS)
 *   rc = stream_send_frame(fb->buf, fb->len)   (plan-mapped sends)
 *   esp_camera_fb_return(fb)                   (ALWAYS)
 *   sent++ | dropped++                          (by rc)
 * Returns true when a frame was consumed and sent; false on
 * timeout or failed send. Host tests call it directly (mirrors
 * capture_loop_iteration); the device wrapper loops it forever.
 */
bool stream_loop_iteration(void);

/* ---------- sender ---------- */

/* Ship one complete binary WS message to the connected viewer:
 *   no viewer / send failure  → -1 (caller: D4 drain-drop-count)
 *   success                   → payload length accepted
 * The sink (ws.h) is installed at /cams viewer accept; with the
 * disconnected stubs this returns -1 and the loop keeps
 * consuming. */
int stream_send_frame(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
