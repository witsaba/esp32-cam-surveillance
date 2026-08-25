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
#include "mock_nvs_flash_link.h"
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include <nvs.h>

#define TAG "health"

/* Forensic reason persistence (R-FW16-1.2, AD4). The DEDICATED
 * namespace survives factory reset BY CONSTRUCTION:
 * config_factory_reset() erases only ns "config" (config.c:191),
 * and the forensic reason is most valuable right after a reset.
 * The dedicated namespace also sidesteps the config schema-version
 * guard entirely. */
#define HEALTH_RECOVERY_NS         "recovery"
#define HEALTH_RECOVERY_KEY        "last_recovery_reason"
#define HEALTH_RECOVERY_REASON_STR "soft_recovery_threshold"

#ifndef UNITY_HOST_BUILD
static void health_task_entry(void *arg);
#endif

/* Persist the recovery reason (AD4): config.c write style, single
 * attempt, ESP_LOGE on failure but recovery proceeds — the trigger
 * must never wedge the device. */
void health_persist_last_recovery_reason(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(HEALTH_RECOVERY_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist: nvs_open(\"%s\") failed: %d",
                 HEALTH_RECOVERY_NS, (int)err);
        return;
    }
    err = nvs_set_str(h, HEALTH_RECOVERY_KEY, HEALTH_RECOVERY_REASON_STR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist: nvs_set_str failed: %d", (int)err);
        nvs_close(h);
        return;
    }
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist: nvs_commit failed: %d", (int)err);
    }
    nvs_close(h);
}

/* Next-boot surfacing (AD5): READONLY read of 64-byte buf; hit →
 * INFO line; NOT_FOUND → silent; other error → warn. Best-effort:
 * never alters boot flow. */
void health_log_last_recovery_reason(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(HEALTH_RECOVERY_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return; /* fresh partition — nothing to surface */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "surface: nvs_open(\"%s\") failed: %d",
                 HEALTH_RECOVERY_NS, (int)err);
        return;
    }

    char buf[64];
    size_t len = sizeof(buf);
    err = nvs_get_str(h, HEALTH_RECOVERY_KEY, buf, &len);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return; /* silent per AD4 */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "surface: nvs_get_str failed: %d", (int)err);
        return;
    }
    ESP_LOGI(TAG, "fw: last_recovery_reason: %s", buf);
}

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
