/* control.c — FW-18 control-task shell: bounded depth-8 command ring
 * (capture_queue_t-shaped dual backend), drop accounting, and the
 * consumer loop that turns queued frames into replies on the viewer
 * sink. The strong control_task_start() symbol lands in T2.3,
 * replacing components/boot/stub_supervision.c.
 *
 * Ownership chain (D3): producers push a heap copy (NUL-terminated,
 * ≤ CONTROL_FRAME_MAX bytes); control_loop_iteration is its SINGLE
 * consumer and frees it after process+emit — one alloc, one free.
 *
 * Drop accounting: every false return from the ring send IS a
 * production drop (the ingest hook and tests are the only
 * producers), so the counter bumps inside the ring op — ruling
 * #3966.2 semantics in one place; nothing goes on the wire.
 */
#include "control.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
/* Host sync hooks — pthread mutex + condvar, CLOCK_MONOTONIC where
 * available (capture.c:46-61 precedent; macOS falls back to the
 * wall clock since pthread_condattr_setclock is absent). */
#include <errno.h>
#include <pthread.h>
#include <time.h>

#if !defined(__APPLE__)
#define CONTROL_QUEUE_HAS_SETCLOCK 1
#define CONTROL_QUEUE_WAIT_CLOCK   CLOCK_MONOTONIC
#else
#define CONTROL_QUEUE_HAS_SETCLOCK 0
#define CONTROL_QUEUE_WAIT_CLOCK   CLOCK_REALTIME
#endif
#else
/* Device sync hooks — FreeRTOS mutex + counting-semaphore-as-condvar
 * (capture.c:239-255 precedent). */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#include "ws.h" /* ws_sink_send_text — reply emission seam */

#ifdef UNITY_HOST_BUILD
/* Host bookkeeping mocks (migrated VERBATIM from the deleted
 * boot/stub_supervision.c:61-65). */
#include "mock_init_returns.h"
#include "mock_supervision_record.h"
#endif

#include "boot.h"      /* BOOT_STEP_SUPERVISION_CONTROL */
#include "boot_priq.h" /* BOOT_TASK_STACK/PRIO_SUPERVISION */

#define TAG "control"

static control_queue_t g_control_queue;
static uint32_t        s_frames_dropped;

/* control_route.c-internal seam (not in the public header). */
void control_registry_reset_for_test(void);

void control_reset_for_test(void)
{
#ifdef UNITY_HOST_BUILD
    memset(&g_control_queue, 0, sizeof(g_control_queue));
    s_frames_dropped = 0;
#endif
    control_registry_reset_for_test();
}

uint32_t control_frames_dropped_get(void)
{
    return s_frames_dropped;
}

control_queue_t *control_queue_for_test(void)
{
    return &g_control_queue;
}

/* Shared pop — caller holds the lock when hooks are armed. */
static void queue_pop_unsafe(control_queue_t *q, void **out)
{
    *out = q->slots[q->head];
    q->head = (q->head + 1) % CONTROL_QUEUE_DEPTH;
    q->count--;
}

#ifdef UNITY_HOST_BUILD

static pthread_mutex_t g_queue_mtx;
static pthread_cond_t  g_queue_cv;
static bool            g_queue_cv_init_done;

/* Arm the module-static ring's sync hooks (idempotent; mirrors
 * capture_queue_hooks_install). */
static void control_queue_hooks_install(control_queue_t *q)
{
    if (!g_queue_cv_init_done) {
#if CONTROL_QUEUE_HAS_SETCLOCK
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

bool control_queue_send_drop_on_full(control_queue_t *q, void *item)
{
    if (!q || !item) return false;
    bool synced = (q->sync_mtx && q->sync_cv);
    if (synced) pthread_mutex_lock((pthread_mutex_t *)q->sync_mtx);
    if (q->count >= CONTROL_QUEUE_DEPTH) {
        if (synced) pthread_mutex_unlock((pthread_mutex_t *)q->sync_mtx);
        if (q == &g_control_queue) s_frames_dropped++;
        return false; /* drop NEWEST — producer never blocks */
    }
    q->slots[q->tail] = item;
    q->tail = (q->tail + 1) % CONTROL_QUEUE_DEPTH;
    q->count++;
    if (synced) {
        pthread_cond_signal((pthread_cond_t *)q->sync_cv);
        pthread_mutex_unlock((pthread_mutex_t *)q->sync_mtx);
    }
    return true;
}

bool control_queue_receive_timeout(void **out, uint32_t timeout_ms)
{
    if (!out) return false;
    control_queue_t *q = &g_control_queue;

    if (!(q->sync_mtx && q->sync_cv)) {
        if (q->count == 0) return false;
        queue_pop_unsafe(q, out);
        return true;
    }

    struct timespec deadline;
    clock_gettime(CONTROL_QUEUE_WAIT_CLOCK, &deadline);
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

static SemaphoreHandle_t g_queue_mutex_handle;
static SemaphoreHandle_t g_queue_sem_handle;

static void control_queue_hooks_install(control_queue_t *q)
{
    if (!g_queue_mutex_handle) {
        g_queue_mutex_handle = xSemaphoreCreateMutex();
        g_queue_sem_handle   = xSemaphoreCreateCounting(
            CONTROL_QUEUE_DEPTH, 0);
    }
    q->sync_mtx = (void *)g_queue_mutex_handle;
    q->sync_cv  = (void *)g_queue_sem_handle;
}

bool control_queue_send_drop_on_full(control_queue_t *q, void *item)
{
    if (!q || !item) return false;
    bool synced = (q->sync_mtx && q->sync_cv);
    if (synced) xSemaphoreTake((SemaphoreHandle_t)q->sync_mtx, portMAX_DELAY);
    if (q->count >= CONTROL_QUEUE_DEPTH) {
        if (synced) xSemaphoreGive((SemaphoreHandle_t)q->sync_mtx);
        if (q == &g_control_queue) s_frames_dropped++;
        return false; /* drop NEWEST — producer never blocks */
    }
    q->slots[q->tail] = item;
    q->tail = (q->tail + 1) % CONTROL_QUEUE_DEPTH;
    q->count++;
    if (synced) {
        xSemaphoreGive((SemaphoreHandle_t)q->sync_cv);
        xSemaphoreGive((SemaphoreHandle_t)q->sync_mtx);
    }
    return true;
}

bool control_queue_receive_timeout(void **out, uint32_t timeout_ms)
{
    if (!out) return false;
    control_queue_t *q = &g_control_queue;

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
        (void)xSemaphoreTake((SemaphoreHandle_t)q->sync_cv,
                             deadline - now);
    }
}

#endif /* UNITY_HOST_BUILD */

bool control_loop_iteration(void)
{
    void *p = NULL;
    if (!control_queue_receive_timeout(&p, CONTROL_RECEIVE_TIMEOUT_MS)) {
        return false; /* bounded idle tick — never blocks forever */
    }

    char *frame      = (char *)p;
    char  reply[CONTROL_FRAME_MAX];
    size_t n = control_frame_process(frame, strlen(frame),
                                     reply, sizeof(reply));
    if (n > 0) {
        esp_err_t r = ws_sink_send_text(reply, n);
        /* Sink disconnected is the normal no-viewer state — log and
         * carry on; the command still ran through the dispatcher. */
        if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "reply send failed: %s", esp_err_to_name(r));
        }
    }
    free(frame); /* single owner — free-once */
    return true;
}

/* ---------- task shell (strong symbol — stub retired) ---------- */

#ifndef UNITY_HOST_BUILD
/* Device wrapper: the loop is bounded inside (receive waits ≤1 s,
 * stream.c:79 precedent) and swallows every error, so it runs
 * forever without wedging. */
static void control_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        (void)control_loop_iteration();
    }
}
#endif

esp_err_t control_task_start(void)
{
#ifdef UNITY_HOST_BUILD
    /* Verbatim bookkeeping migration from boot/stub_supervision.c:
     * honour mock_init_returns_get for the FW-03.2 fail-loud
     * regression + record the supervision role for the FW-03.1
     * ordering test (test_boot_order still asserts
     * [health, capture, stream, control]). */
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_SUPERVISION_CONTROL);
    if (forced != ESP_OK) return forced;
    mock_supervision_record("control");
#endif

    ESP_LOGI(TAG, "control_task_start: spawn FreeRTOS control dispatcher");

    /* Arm the cross-task sync hooks BEFORE spawning (capture.c:246
     * precedent) so producers on the httpd worker can wake the
     * consumer. Stack-instantiated test queues stay NULL-hooked. */
    control_queue_hooks_install(&g_control_queue);

#ifndef UNITY_HOST_BUILD
    BaseType_t ret = xTaskCreate(control_task_entry, "control",
                                 BOOT_TASK_STACK_SUPERVISION, NULL,
                                 BOOT_TASK_PRIO_SUPERVISION, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed: ret=%d — control loop NOT "
                      "running", (int)ret);
        return ESP_FAIL;
    }
#endif
    return ESP_OK;
}
