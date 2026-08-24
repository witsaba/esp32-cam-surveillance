/* stream.h — public API for the FW-15 stream component.
 *
 * Consumes the FW-11 depth-2 capture queue and ships each camera
 * frame as a binary WebSocket message (REQ-ST-001/002), fragment-
 * ing frames larger than CONFIG_FIRMWARE_WS_BUFFER_SIZE via
 * send_bin_partial → send_cont_msg* → send_fin (REQ-ST-003).
 *
 * Ownership (REQ-ST-005, amended capture.h contract): the stream
 * task owns the frame buffer from receive until AFTER the send
 * attempt — esp_camera_fb_return fires on success AND failure.
 *
 * Split per design: pure planner (stream_fragment.{c,h}) + thin
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

/* Map the fragment plan onto the IDF binary-send verbs and ship
 * one complete WS message:
 *   len ≤ chunk  → 1 × send_bin                      (REQ-ST-001)
 *   len > chunk  → send_bin_partial(first) →
 *                  send_cont_msg*(middles+last) →
 *                  send_fin()                        (REQ-ST-003)
 * Every slice stays ≤ CONFIG_FIRMWARE_WS_BUFFER_SIZE (zero-copy
 * into the caller's buffer). Returns the number of payload bytes
 * accepted (> 0 / == len) on success, -1 if any send attempt or
 * argument fails. On failure mid-sequence the message is left
 * unterminated — documented, accepted (IDF resets message state
 * on the next leading send). */
int stream_send_frame(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
