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

#include <string.h>

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
/* Host — redirect esp_camera_fb_get / fb_return to the mock
 * triplet. mock_supervision_record + mock_init_returns come
 * in via the boot mocks. */
#include "mock_esp_camera_link.h"
#include "mock_supervision_record.h"
#include "mock_init_returns.h"

/* FW-15 (D2) — host sync hooks: pthread mutex + condvar with
 * CLOCK_MONOTONIC so pthread_cond_timedwait is immune to wall
 * clock adjustments. macOS has no pthread_condattr_setclock and
 * its condvar waits against the wall clock, so there we use a
 * CLOCK_REALTIME deadline with an un-attributed condvar (host
 * test infra only — device uses FreeRTOS primitives). */
#include <errno.h>
#include <pthread.h>
#include <time.h>

#if !defined(__APPLE__)
#define CAPTURE_QUEUE_HAS_SETCLOCK 1
#define CAPTURE_QUEUE_WAIT_CLOCK   CLOCK_MONOTONIC
#else
#define CAPTURE_QUEUE_HAS_SETCLOCK 0
#define CAPTURE_QUEUE_WAIT_CLOCK   CLOCK_REALTIME
#endif
#else
/* Device — link the real esp32-camera managed component
 * via REQUIRES in the component CMakeLists.txt. */
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
/* FW-15 (D2) — device sync hooks: FreeRTOS mutex +
 * counting-semaphore-as-condvar. */
#include "freertos/semphr.h"
#endif

#include "boot_priq.h" /* BOOT_TASK_STACK_SUPERVISION + BOOT_TASK_PRIO_SUPERVISION */
#include "boot.h"

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

/* Host-only reset helper (test seam). Mirrors the mock
 * triplet's reset() pattern. On device the counters are
 * BSS-zeroed at boot + the queue is created at task
 * start; this function is a no-op there. */
void capture_counters_reset_for_test(void)
{
#ifdef UNITY_HOST_BUILD
    memset(&g_capture_queue, 0, sizeof(g_capture_queue));
    memset(&g_capture_counters, 0, sizeof(g_capture_counters));
#endif
}

/* FW-15 host-only test seam: the module-static ring, so host
 * tests can play producer into the SAME queue that
 * capture_queue_receive_timeout consumes. */
capture_queue_t *capture_queue_for_test(void)
{
    return &g_capture_queue;
}

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
 * FW-15 sync hooks (design D2): when `q->sync_mtx` and
 * `q->sync_cv` are non-NULL the queue ops take the mutex and
 * signal the condvar (host) / give the counting semaphore
 * (device) so a blocked consumer wakes on push. Stack
 * queues with NULL hooks stay lock-free + pure.
 */

/* Shared pop — caller holds the lock when hooks are armed. */
static void queue_pop_unsafe(capture_queue_t *q, void **out)
{
    *out = q->slots[q->head];
    q->head = (q->head + 1) % MOCK_CAPTURE_QUEUE_DEPTH;
    q->count--;
}

#ifdef UNITY_HOST_BUILD

static pthread_mutex_t g_queue_mtx;
static pthread_cond_t  g_queue_cv;
static bool            g_queue_cv_init_done;

static bool capture_queue_receive_timeout_on(capture_queue_t *q,
                                             void **out,
                                             uint32_t timeout_ms);

/* Arm the module-static queue's sync hooks. Called from
 * capture_task_start; idempotent per reset cycle (the cond
 * is re-initialised only once; capture_counters_reset_for
 * _test clears the POINTERS, not the statics). */
static void capture_queue_hooks_install(capture_queue_t *q)
{
    if (!g_queue_cv_init_done) {
#if CAPTURE_QUEUE_HAS_SETCLOCK
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&g_queue_cv, &attr);
        pthread_condattr_destroy(&attr);
#else
        pthread_cond_init(&g_queue_cv, NULL);
#endif
        pthread_mutex_init(&g_queue_mtx, NULL);
        g_queue_cv_init_done = true;
    }
    q->sync_mtx = &g_queue_mtx;
    q->sync_cv  = &g_queue_cv;
}

bool capture_queue_send_drop_on_full(capture_queue_t *q, void *fb)
{
    if (!q || !fb) return false;
    bool synced = (q->sync_mtx && q->sync_cv);
    if (synced) pthread_mutex_lock((pthread_mutex_t *)q->sync_mtx);
    if (q->count >= MOCK_CAPTURE_QUEUE_DEPTH) {
        if (synced) pthread_mutex_unlock((pthread_mutex_t *)q->sync_mtx);
        return false; /* full — caller drops + returns buffer */
    }
    q->slots[q->tail] = fb;
    q->tail = (q->tail + 1) % MOCK_CAPTURE_QUEUE_DEPTH;
    q->count++;
    if (synced) {
        pthread_cond_signal((pthread_cond_t *)q->sync_cv);
        pthread_mutex_unlock((pthread_mutex_t *)q->sync_mtx);
    }
    return true;
}

bool capture_queue_receive_timeout(void **out, uint32_t timeout_ms)
{
    return capture_queue_receive_timeout_on(&g_capture_queue,
                                            out, timeout_ms);
}

/* Host receive: bounded wait on the static ring. NULL-hook
 * queues fall back to a non-blocking pop (fast path for pure
 * stack tests). */
static bool capture_queue_receive_timeout_on(capture_queue_t *q,
                                             void **out,
                                             uint32_t timeout_ms)
{
    if (!q || !out) return false;
    if (!(q->sync_mtx && q->sync_cv)) {
        if (q->count == 0) return false;
        queue_pop_unsafe(q, out);
        return true;
    }

    struct timespec deadline;
    clock_gettime(CAPTURE_QUEUE_WAIT_CLOCK, &deadline);
    deadline.tv_sec  += timeout_ms / 1000U;
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock((pthread_mutex_t *)q->sync_mtx);
    int rc = 0;
    while (q->count == 0 && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait((pthread_cond_t *)q->sync_cv,
                                    (pthread_mutex_t *)q->sync_mtx,
                                    &deadline);
    }
    if (q->count == 0) {
        pthread_mutex_unlock((pthread_mutex_t *)q->sync_mtx);
        return false;
    }
    queue_pop_unsafe(q, out);
    pthread_mutex_unlock((pthread_mutex_t *)q->sync_mtx);
    return true;
}

#else /* device build */

/* Device hooks: mutex guards the ring; a counting semaphore
 * (max depth, start 0) plays condvar. The producer gives it
 * under lock on every successful push; the consumer takes
 * it with the remaining budget between re-checks. */
static SemaphoreHandle_t g_queue_mutex_handle;
static SemaphoreHandle_t g_queue_sem_handle;

static void capture_queue_hooks_install(capture_queue_t *q)
{
    if (!g_queue_mutex_handle) {
        g_queue_mutex_handle = xSemaphoreCreateMutex();
        g_queue_sem_handle   = xSemaphoreCreateCounting(
            MOCK_CAPTURE_QUEUE_DEPTH, 0);
    }
    q->sync_mtx = (void *)g_queue_mutex_handle;
    q->sync_cv  = (void *)g_queue_sem_handle;
}

bool capture_queue_send_drop_on_full(capture_queue_t *q, void *fb)
{
    if (!q || !fb) return false;
    bool synced = (q->sync_mtx && q->sync_cv);
    if (synced) xSemaphoreTake((SemaphoreHandle_t)q->sync_mtx, portMAX_DELAY);
    if (q->count >= MOCK_CAPTURE_QUEUE_DEPTH) {
        if (synced) xSemaphoreGive((SemaphoreHandle_t)q->sync_mtx);
        return false; /* full — caller drops + returns buffer */
    }
    q->slots[q->tail] = fb;
    q->tail = (q->tail + 1) % MOCK_CAPTURE_QUEUE_DEPTH;
    q->count++;
    if (synced) {
        xSemaphoreGive((SemaphoreHandle_t)q->sync_cv);
        xSemaphoreGive((SemaphoreHandle_t)q->sync_mtx);
    }
    return true;
}

bool capture_queue_receive_timeout(void **out, uint32_t timeout_ms)
{
    capture_queue_t *q = &g_capture_queue;
    if (!out) return false;
    if (!(q->sync_mtx && q->sync_cv)) {
        if (q->count == 0) return false;
        queue_pop_unsafe(q, out);
        return true;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        xSemaphoreTake((SemaphoreHandle_t)q->sync_mtx, portMAX_DELAY);
        if (q->count > 0) {
            queue_pop_unsafe(q, out);
            xSemaphoreGive((SemaphoreHandle_t)q->sync_mtx);
            return true;
        }
        xSemaphoreGive((SemaphoreHandle_t)q->sync_mtx);

        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return false;
        /* Wait for a push (or the remaining budget). */
        (void)xSemaphoreTake((SemaphoreHandle_t)q->sync_cv,
                             deadline - now);
    }
}

#endif /* UNITY_HOST_BUILD */

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
    void *fb = esp_camera_fb_get();
    if (!fb) return; /* driver not ready — retry next tick */

    /* FW-11.5 closing-check log: at the first fb_get, log
     * psram_before + psram_after so the device-side verifier
     * can grep for the literal "psram_before=<N> psram_after=<M>"
     * and confirm the frame buffer landed in PSRAM
     * (MALLOC_CAP_SPIRAM), not internal SRAM. Mirrors the
     * FW-10.4 "psram_size=<N> bytes" log pattern.
     *
     * The .len access requires the real camera_fb_t struct
     * definition. On host this comes from mock_esp_camera.h
     * (already included via mock_esp_camera_link.h); on
     * device from esp_camera.h (already included above).
     * We cast through the appropriate pointer type. */
#ifdef UNITY_HOST_BUILD
    /* Host — use the mock's struct definition. */
    extern size_t heap_caps_get_free_size(uint32_t caps);
    camera_fb_t *fb_host = (camera_fb_t *)fb;
    if (g_capture_counters.frames_captured == 0 &&
        g_capture_counters.fb_drops == 0) {
        size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
                              + fb_host->len;
        size_t psram_after  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "psram_before=%u psram_after=%u",
                 (unsigned)psram_before, (unsigned)psram_after);
    }
#else
    /* Device — use the real esp32-camera struct definition. */
    extern size_t heap_caps_get_free_size(uint32_t caps);
    camera_fb_t *fb_dev = (camera_fb_t *)fb;
    if (g_capture_counters.frames_captured == 0 &&
        g_capture_counters.fb_drops == 0) {
        size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
                              + fb_dev->len;
        size_t psram_after  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "psram_before=%u psram_after=%u",
                 (unsigned)psram_before, (unsigned)psram_after);
    }
#endif

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

/* Periodic progress log — every N iterations (N=25 = 5s @ 5fps).
 * Gives the device-side smoke a visible heartbeat so the operator
 * can confirm the loop is alive without needing FW-13.6's status
 * frame. Removes itself as soon as FW-13.6 lands (which emits
 * structured `{"type":"status", ..., "fb_drops":N, ...}` every
 * 30s and is the canonical observer). Until then, this is the
 * only signal that capture is iterating at 5 Hz. */
#define CAPTURE_PROGRESS_EVERY 25u

static void capture_task_entry(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    for (;;) {
        capture_loop_iteration(&g_capture_queue, &g_capture_counters);
        if ((++tick % CAPTURE_PROGRESS_EVERY) == 0) {
            ESP_LOGI(TAG, "progress: frames_captured=%u fb_drops=%u tick=%u",
                     (unsigned)g_capture_counters.frames_captured,
                     (unsigned)g_capture_counters.fb_drops,
                     (unsigned)tick);
        }
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

    /* FW-15 (D2): arm the cross-task sync hooks on the
     * module-static queue so the stream task can consume it
     * with a bounded wait. Stack-instantiated test queues are
     * unaffected (their hooks stay NULL). */
    capture_queue_hooks_install(&g_capture_queue);

#ifndef UNITY_HOST_BUILD
    BaseType_t ret = xTaskCreate(capture_task_entry, "capture",
                                 BOOT_TASK_STACK_SUPERVISION, NULL,
                                 BOOT_TASK_PRIO_SUPERVISION, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed: ret=%d — capture loop NOT running", (int)ret);
        return ESP_FAIL;
    }
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
