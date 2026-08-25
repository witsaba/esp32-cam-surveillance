/* stub_supervision.c — stubs for the remaining supervision-task
 * interfaces FW-03 owns as call-sites only. (The stream stub was
 * DELETED in FW-15 — components/stream/stream.c provides the real
 * strong symbol; the capture stub was deleted in FW-11. The health
 * stub was DELETED in FW-16 — components/health/health.c provides
 * the real strong symbol.)
 *
 * Each stub returns ESP_OK on the green path; on the host build it
 * consults `mock_init_returns_get(step)` first AND records the
 * call via `mock_supervision_record(role)` so the FW-03.1 ordering
 * test can assert the FR-1 sequence.
 *
 * On the device build, each stub creates a no-op FreeRTOS task
 * named "stub_<role>" via `xTaskCreate(stub_task_body, …)`. The
 * task body suspends itself forever (`vTaskSuspend(NULL)`) so the
 * task is observable at FW-23 via `ps` but does not consume CPU
 * cycles. A missing real impl is therefore visible at FW-23 (no
 * tasks running) instead of silently passing with no tasks.
 *
 * `xTaskCreate` is gated by `#ifndef UNITY_HOST_BUILD` because
 * FreeRTOS is not linked into the host build.
 */
#include "boot.h"
#include "boot_priq.h"

#include "esp_log.h"

#ifndef UNITY_HOST_BUILD
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#ifdef UNITY_HOST_BUILD
#include "mock_init_returns.h"
#include "mock_supervision_record.h"
#endif

static const char *TAG = "boot";

#ifndef UNITY_HOST_BUILD
/* No-op FreeRTOS task body. The task suspends itself immediately
 * so it never consumes CPU. The named task is observable at
 * runtime via `ps`-style debug — a missing real impl (where the
 * stub is supposed to be replaced) is therefore visible. */
static void stub_task_body(void *arg) {
    (void)arg;
    vTaskSuspend(NULL);
}
#endif

/* health_task_start — STUB DELETED in FW-16 (T-13-I pattern).
 * The real implementation lives in components/health/health.c and
 * provides the strong symbol; the linker resolves boot.c:207's
 * call directly. `make test-stub` guards against any duplicate-
 * definition regression. The host contract (mock_init_returns
 _get short-circuit + mock_supervision_record("health")) moved
 * verbatim into health.c — test_boot_order.c:84-96 still asserts
 * the [health, capture, stream, control] sequence. */

esp_err_t control_task_start(void) {
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_SUPERVISION_CONTROL);
    if (forced != ESP_OK) return forced;
    mock_supervision_record("control");
#endif
    ESP_LOGI(TAG, "stub: control_task_start  // FW-18: real impl lands in control task");
#ifndef UNITY_HOST_BUILD
    xTaskCreate(stub_task_body, "stub_control",
                BOOT_TASK_STACK_SUPERVISION, NULL,
                BOOT_TASK_PRIO_SUPERVISION, NULL);
#endif
    return ESP_OK;
}