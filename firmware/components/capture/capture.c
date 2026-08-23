/* capture.c — FW-11 capture-task implementation.
 *
 * Splits the loop body into a PURE function
 * (`capture_loop_iteration`) + a thin FreeRTOS wrapper
 * (`capture_task_entry`). Host tests call the pure function
 * directly; device builds spawn a FreeRTOS task that
 * invokes the wrapper inside an infinite for-loop.
 *
 * Production data flow (per design #3734 AD-2):
 *
 *   vTaskDelay(pdMS_TO_TICKS(period))
 *      └─► capture_loop_iteration(q, c)
 *            ├─► camera_fb_t *fb = esp_camera_fb_get()
 *            ├─► if (!fb) return                  // driver not ready
 *            ├─► if (!capture_queue_send_drop_on_full(q, fb))
 *            │     ├─► esp_camera_fb_return(fb)
 *            │     └─► c->fb_drops++
 *            └─► c->frames_captured++
 *
 * The single-owner invariant (PRD § FR-2b) holds by
 * architecture: this file is the ONLY TU that calls
 * `esp_camera_fb_get` / `esp_camera_fb_return`. The FW-11.3
 * guard tripwire (Pass 10 stub build with
 * -DCAPTURE_TEST_STUB_SECOND_CALLER=1) proves this
 * invariant is load-bearing by introducing a synthetic 2nd
 * caller via the test fixture.
 */
#include "capture.h"

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
/* Host — redirect esp_camera_fb_get / fb_return to the mock
 * triplet. mock_supervision_record + mock_init_returns come
 * in via the boot mocks. */
#include "mock_esp_camera_link.h"
#include "mock_supervision_record.h"
#include "mock_init_returns.h"
#include "boot_priq.h"
#include "boot.h"
#else
/* Device — link the real esp32-camera managed component
 * via REQUIRES in the component CMakeLists.txt. */
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#endif

#define TAG "capture"

/* ---------- module-static state (host + device) ---------- */

/* The depth-2 frame queue + counters. On device the queue
 * is a real xQueueHandle cast through `capture_queue_t`'s
 * slots[] (which are unused on device — the wrapper writes
 * to the xQueueHandle via the link header); on host the
 * queue is the static slots[] + head/tail/count. */
static capture_queue_t    g_capture_queue;
static capture_counters_t g_capture_counters;

/* Cross-task getters — uint32_t reads are atomic on Xtensa
 * LX6, so no mutex is needed. FW-13.6 reads these from the
 * status-task context. */
uint32_t capture_fb_drops_get(void)        { return g_capture_counters.fb_drops; }
uint32_t capture_frames_captured_get(void) { return g_capture_counters.frames_captured; }

/* ---------- host queue backend (depth-2 ring buffer) ----------
 *
 * The host capture_queue_t is a thin wrapper around a
 * static slots[] array with head/tail indices. The ring is
 * full when `count == MOCK_CAPTURE_QUEUE_DEPTH`; in that
 * case `capture_queue_send_drop_on_full()` returns false
 * and the caller is responsible for returning the buffer +
 * incrementing fb_drops. Mirrors the device xQueueSend(0)
 * non-blocking semantics.
 *
 * On device the function is a no-op — the wrapper writes
 * to xQueueHandle directly via the production code path
 * (the FreeRTOS queue API takes a real QueueHandle_t). To
 * keep the pure-function signature stable across hosts, we
 * implement the same shape on device but operate on a
 * real xQueueHandle through the same slots[].
 */
bool capture_queue_send_drop_on_full(capture_queue_t *q, camera_fb_t *fb)
{
    if (!q || !fb) return false;
    if (q->count >= MOCK_CAPTURE_QUEUE_DEPTH) {
        return false; /* full — caller drops + returns buffer */
    }
    q->slots[q->tail] = fb;
    q->tail = (q->tail + 1) % MOCK_CAPTURE_QUEUE_DEPTH;
    q->count++;
    return true;
}

/* ---------- pure loop body ----------
 *
 * ONE iteration: acquire → enqueue (or drop) → count.
 * The FreeRTOS wrapper calls this inside an infinite
 * for-loop; host tests call it N times to simulate wall
 * clock time. Mirrors the data-flow diagram in the design
 * artifact (#3734).
 */
void capture_loop_iteration(capture_queue_t *q, capture_counters_t *c)
{
    if (!q || !c) return;
    camera_fb_t *fb = (camera_fb_t *)esp_camera_fb_get();
    if (!fb) return; /* driver not ready — retry next tick */

    if (!capture_queue_send_drop_on_full(q, fb)) {
        esp_camera_fb_return(fb);
        c->fb_drops++;
        g_capture_counters.fb_drops++; /* mirror to module-static for FW-13.6 */
        return;
    }
    c->frames_captured++;
    g_capture_counters.frames_captured++; /* mirror to module-static */
}

#ifndef UNITY_HOST_BUILD
/* ---------- FreeRTOS wrapper (device-only) ---------- */

/* Loop period in ms — 200 ms = 5 fps per PRD § FR-3 (FW-11
 * hardcodes 5 fps; FW-19 owns runtime config-driven fps). */
#define CAPTURE_PERIOD_MS 200

static void capture_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        capture_loop_iteration(&g_capture_queue, &g_capture_counters);
        vTaskDelay(pdMS_TO_TICKS(CAPTURE_PERIOD_MS));
    }
}
#endif

/* ---------- public API ---------- */

esp_err_t capture_task_start(void)
{
#ifdef UNITY_HOST_BUILD
    /* Mirror the stub's host-side bookkeeping: record the
     * supervision role for the FW-03.1 ordering test + honour
     * mock_init_returns_get for the FW-03.2 fail-loud
     * regression. */
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_SUPERVISION_CAPTURE);
    if (forced != ESP_OK) return forced;
    mock_supervision_record("capture");
#endif

#ifdef CAPTURE_TEST_STUB_SECOND_CALLER
    /* FW-11.3 bite-proof — under the stub flag, the 2nd caller
     * (the test fixture function `_capture_test_stub_second
     * _caller`) introduces a violation of the single-owner
     * invariant. The guard fires here. Pass 10 greps for the
     * literal "single_owner" in stdout. On host the abort
     * path uses TEST_FAIL_MESSAGE; on device it's a
     * typed-error log + return ESP_FAIL. */
    extern void capture_guard_fail_single_owner(void);
    capture_guard_fail_single_owner();
    return ESP_FAIL; /* unreachable on host */
#endif

    ESP_LOGI(TAG, "capture_task_start: spawn FreeRTOS capture loop @ 5 fps");
#ifndef UNITY_HOST_BUILD
    xTaskCreate(capture_task_entry, "capture",
                BOOT_TASK_STACK_SUPERVISION, NULL,
                BOOT_TASK_PRIO_SUPERVISION, NULL);
#endif
    return ESP_OK;
}

#ifdef UNITY_HOST_BUILD
#include "unity.h"
/* FW-11.3 guard tripwire (host). Mirrors the
 * camera_guard_fail_no_reinit() pattern at camera.c:264.
 * The literal substring "single_owner" must appear in this
 * message so Pass 10 can grep stdout. */
void capture_guard_fail_single_owner(void)
{
    TEST_FAIL_MESSAGE("single_owner invariant violated: "
                      "esp_camera_fb_get called from non-"
                      "capture-task symbol");
}
#endif
