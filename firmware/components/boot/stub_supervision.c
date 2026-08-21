/* stub_supervision.c — stubs for the 4 supervision-task interfaces
 * FW-03 owns as call-sites only.
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

esp_err_t health_task_start(void) {
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_SUPERVISION_HEALTH);
    if (forced != ESP_OK) return forced;
    mock_supervision_record("health");
#endif
    ESP_LOGI(TAG, "stub: health_task_start  // FW-16: real impl lands in supervision/health");
#ifndef UNITY_HOST_BUILD
    xTaskCreate(stub_task_body, "stub_health",
                BOOT_TASK_STACK_SUPERVISION, NULL,
                BOOT_TASK_PRIO_SUPERVISION, NULL);
#endif
    return ESP_OK;
}

esp_err_t capture_task_start(void) {
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_SUPERVISION_CAPTURE);
    if (forced != ESP_OK) return forced;
    mock_supervision_record("capture");
#endif
    ESP_LOGI(TAG, "stub: capture_task_start  // FW-11: real impl lands in capture task");
#ifndef UNITY_HOST_BUILD
    xTaskCreate(stub_task_body, "stub_capture",
                BOOT_TASK_STACK_SUPERVISION, NULL,
                BOOT_TASK_PRIO_SUPERVISION, NULL);
#endif
    return ESP_OK;
}

esp_err_t stream_task_start(void) {
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_SUPERVISION_STREAM);
    if (forced != ESP_OK) return forced;
    mock_supervision_record("stream");
#endif
    ESP_LOGI(TAG, "stub: stream_task_start  // FW-15: real impl lands in stream task");
#ifndef UNITY_HOST_BUILD
    xTaskCreate(stub_task_body, "stub_stream",
                BOOT_TASK_STACK_SUPERVISION, NULL,
                BOOT_TASK_PRIO_SUPERVISION, NULL);
#endif
    return ESP_OK;
}

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