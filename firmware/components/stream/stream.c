/* stream.c — FW-15 stream-task implementation.
 *
 * Mirrors the capture component's split: a PURE-ish loop
 * iteration (`stream_loop_iteration`) + a thin FreeRTOS wrapper
 * (`stream_task_entry`, device-only). Host tests call the loop
 * function directly.
 *
 * Production data flow (per design §Data Flow):
 *
 *   for (;;)                                    (device wrapper)
 *      └─► stream_loop_iteration()
 *            ├─► capture_queue_receive_timeout(&fb, 1000 ms)
 *            ├─► stream_send_frame(fb->buf, fb->len)   (plan-mapped)
 *            ├─► esp_camera_fb_return(fb)       ◄── ALWAYS (REQ-ST-005:
 *            │        consumer-owned post-receive; success OR failure)
 *            └─► sent++ | dropped++                (D4 drain-drop-count)
 *                  ESP_LOGI "stream: frame len=… parts=… sent=… dropped=…"
 *                  (greppable device-log evidence seam, REQ-ST-008/009)
 *
 * Counters are stream-OWNED atomic u32s — deliberately separate
 * from capture_fb_drops_get() so FW-13.6 status semantics stay
 * producer-only.
 */
#include "stream.h"

#include <string.h>

#include "esp_log.h"

#include "capture.h"
#include "config.h"
#include "stream_fragment.h"
#include "ws.h" /* ws_handle_get() via the sender */

#ifdef UNITY_HOST_BUILD
/* Host — redirect esp_camera_fb_return to the mock triplet +
 * pull the supervision/init-returns mocks for task_start. */
#include "mock_esp_camera_link.h"
#include "mock_supervision_record.h"
#include "mock_init_returns.h"
#else
/* Device — link the real esp32-camera managed component. */
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "boot_priq.h" /* BOOT_TASK_STACK_SUPERVISION + PRIO */
#include "boot.h"      /* BOOT_STEP_SUPERVISION_STREAM */

#define TAG "stream"

/* Bounded receive budget (design: fixed constant, NO new Kconfig
 * tunable in scope). Mostly the loop sleeps here waiting for the
 * capture task's next frame. */
#define STREAM_RECEIVE_TIMEOUT_MS 1000

/* ---------- module-static state ---------- */

static uint32_t s_frames_sent;
static uint32_t s_frames_dropped;

uint32_t stream_frames_sent_get(void)    { return s_frames_sent; }
uint32_t stream_frames_dropped_get(void) { return s_frames_dropped; }

void stream_counters_reset_for_test(void)
{
#ifdef UNITY_HOST_BUILD
    s_frames_sent    = 0;
    s_frames_dropped = 0;
#endif
}

/* ---------- loop iteration ---------- */

bool stream_loop_iteration(void)
{
    void *p = NULL;
    if (!capture_queue_receive_timeout(&p, STREAM_RECEIVE_TIMEOUT_MS)) {
        return false; /* idle tick — bounded, never forever */
    }
    camera_fb_t *fb = (camera_fb_t *)p;

    int rc = stream_send_frame(fb->buf, fb->len);

    /* Consumer owns the buffer from receive until NOW — return it
     * exactly once, success OR failure (REQ-ST-005). */
    esp_camera_fb_return(fb);

    size_t parts = stream_fragment_count(fb->len,
                                         CONFIG_FIRMWARE_WS_BUFFER_SIZE);
    if (rc < 0) {
        /* D4 drain-drop-count: abort happened inside the sender;
         * count the drop and CONTINUE so the producer never
         * blocks on a full queue. */
        s_frames_dropped++;
        ESP_LOGI(TAG, "stream: frame len=%u parts=%u sent=%u dropped=%u",
                 (unsigned)fb->len, (unsigned)parts,
                 (unsigned)s_frames_sent, (unsigned)s_frames_dropped);
        return false;
    }

    s_frames_sent++;
    ESP_LOGI(TAG, "stream: frame len=%u parts=%u sent=%u dropped=%u",
             (unsigned)fb->len, (unsigned)parts,
             (unsigned)s_frames_sent, (unsigned)s_frames_dropped);
    return true;
}

/* ---------- public API ---------- */

#ifndef UNITY_HOST_BUILD
static void stream_task_entry(void *arg);
#endif

esp_err_t stream_task_start(void)
{
#ifdef UNITY_HOST_BUILD
    /* Mirror the deleted stub's host-side bookkeeping: record the
     * supervision role for the FW-03.1 ordering test + honour
     * mock_init_returns_get for the FW-03.2 fail-loud regression. */
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_SUPERVISION_STREAM);
    if (forced != ESP_OK) return forced;
    mock_supervision_record("stream");
#endif

    ESP_LOGI(TAG, "stream_task_start: spawn FreeRTOS stream loop");

#ifndef UNITY_HOST_BUILD
    BaseType_t ret = xTaskCreate(stream_task_entry, "stream",
                                 BOOT_TASK_STACK_SUPERVISION, NULL,
                                 BOOT_TASK_PRIO_SUPERVISION, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed: ret=%d — stream loop NOT running",
                 (int)ret);
        return ESP_FAIL;
    }
#endif
    return ESP_OK;
}

#ifndef UNITY_HOST_BUILD
/* ---------- FreeRTOS wrapper (device-only) ---------- */

static void stream_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        /* Bounded inside: receive waits ≤1 s; failed sends are
         * counted and drained without backoff. No extra delay —
         * the queue IS the pacing mechanism. */
        (void)stream_loop_iteration();
    }
}
#endif
