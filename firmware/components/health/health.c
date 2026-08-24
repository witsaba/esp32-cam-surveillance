/* health.c — FW-16 soft-recovery health task (R-FW16-1.1/1.2/1.3).
 *
 * Owns the strong `health_task_start()` symbol taken over from the
 * FW-15-era stub (stub_supervision.c body deleted). Mirrors the
 * stream/capture component split: thin FreeRTOS glue HERE, pure
 * window logic in health_window.c.
 *
 * Failure semantics (design AD2): the task subscribes
 * WIFI_EVT_STA_DISCONNECTED (ingest) + WIFI_EVT_STA_GOT_IP
 * (episode boundary) via wifi_event_subscribe and NEVER rebuilds
 * loops — counted failures are Wi-Fi STA disconnect events
 * observed by this task (device-as-server reality since PR #21).
 *
 * Startup log contract: `fw: health_task_start fails=<n>
 * window_min=<n>` at INFO under TAG `health`.
 */
#include "health.h"

#include "esp_log.h"

#include "boot.h"      /* BOOT_STEP_SUPERVISION_HEALTH */
#include "boot_priq.h" /* BOOT_TASK_STACK_SUPERVISION + PRIO */

/* Kconfig mirrors (led.h pattern): device builds resolve via
 * sdkconfig.h; the host build passes -D cflags; these #ifndef
 * fallbacks keep standalone compiles honest. */
#ifndef CONFIG_FIRMWARE_SOFT_RECOVERY_FAILS
#define CONFIG_FIRMWARE_SOFT_RECOVERY_FAILS 30
#endif
#ifndef CONFIG_FIRMWARE_SOFT_RECOVERY_WINDOW_MIN
#define CONFIG_FIRMWARE_SOFT_RECOVERY_WINDOW_MIN 10
#endif

#ifdef UNITY_HOST_BUILD
#include "mock_init_returns.h"
#include "mock_supervision_record.h"
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define TAG "health"

#ifndef UNITY_HOST_BUILD
static void health_task_entry(void *arg);
#endif

esp_err_t health_task_start(void)
{
#ifdef UNITY_HOST_BUILD
    /* Host contract VERBATIM from the deleted stub: honour the
     * forced-error slot first (short-circuit WITHOUT recording),
     * then record the supervision role for the FW-03.1 ordering
     * test. */
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_SUPERVISION_HEALTH);
    if (forced != ESP_OK) return forced;
    mock_supervision_record("health");
#endif

    ESP_LOGI(TAG, "fw: health_task_start fails=%u window_min=%u",
             (unsigned)CONFIG_FIRMWARE_SOFT_RECOVERY_FAILS,
             (unsigned)CONFIG_FIRMWARE_SOFT_RECOVERY_WINDOW_MIN);

#ifndef UNITY_HOST_BUILD
    BaseType_t ret = xTaskCreate(health_task_entry, "health",
                                 BOOT_TASK_STACK_SUPERVISION, NULL,
                                 BOOT_TASK_PRIO_SUPERVISION, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed: ret=%d — health task NOT running",
                 (int)ret);
        return ESP_FAIL;
    }
#endif
    return ESP_OK;
}

#ifndef UNITY_HOST_BUILD
/* Device task body. T3 lands the takeover shell only: the event
 * subscriptions + recovery wiring arrive with T6. Suspending keeps
 * the task observable via `ps` (FW-23) without burning cycles —
 * identical semantics to the stub it replaces. */
static void health_task_entry(void *arg)
{
    (void)arg;
    vTaskSuspend(NULL);
}
#endif
