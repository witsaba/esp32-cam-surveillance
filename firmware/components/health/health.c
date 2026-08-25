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
#include "esp_system.h" /* esp_restart (device; host shim + redirect) */
#include "esp_timer.h"  /* µs clock feeding the sliding window */

#include "health_window.h"
#include "led.h"
#include "wifi.h"       /* wifi_event_subscribe seam */
#include "wifi_event.h" /* WIFI_EVT_* ids + wifi_event_cb_t shape */

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
#include "mock_esp_timer_link.h"
#include "mock_esp_system_link.h"
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

/* ---------- trigger machinery (R-FW16-1.1, AD2/AD5/AD6) ---------- */

/* Live window + tunables. Tunables are consumed from sdkconfig.h /
 * host -D cflags ONCE at task start (AD6 single conversion point). */
static health_window_t s_win;
static int64_t         s_window_us;
static uint32_t        s_threshold;
/* Trigger latch: the recovery sequence runs at most ONCE per boot
 * (idempotent — post-restart noise must not re-fire). */
static bool            s_recovering;

/* AD6: window_us = WINDOW_MIN * 60 * 1e6 in one static helper so
 * the int64 promotion happens in exactly one place. */
static int64_t health_window_us_from_config(void)
{
    return (int64_t)CONFIG_FIRMWARE_SOFT_RECOVERY_WINDOW_MIN * 60LL *
           1000000LL;
}

/* Recovery-complete cb: fired by the LED driver's 3 s one-shot after
 * led_set_state(LED_STATE_SOFT_RECOVERY). esp_restart() is safe from
 * the esp_timer task context (boot.c:83 precedent). */
static void health_recovery_complete_cb(void)
{
    esp_restart();
}

/* Ingest entry point (the ONLY evaluation path in production —
 * R-FW16-1.3 event-driven-only). Episode-latched record; on
 * threshold crossing, strictly: ① persist reason → ② SOFT_RECOVERY
 * LED (arms the one-shot) → (one-shot later fires ③ the registered
 * completion cb → ④ esp_restart). */
static void on_sta_disconnected(void *arg, const char *event_base,
                                int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    if (s_recovering) {
        return; /* already fired this boot */
    }
    size_t n = health_window_record(&s_win, esp_timer_get_time(),
                                    s_window_us);
    if ((uint32_t)n >= s_threshold) {
        s_recovering = true;
        health_persist_last_recovery_reason();          /* ① */
        (void)led_set_state(LED_STATE_SOFT_RECOVERY);   /* ② arms ③④ */
    }
}

/* GOT_IP closes the outage episode so the next drop counts as a NEW
 * failure (AD2). Never evaluates the trigger. */
static void on_got_ip(void *arg, const char *event_base,
                      int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    health_window_mark_reconnected(&s_win);
}

/* Common wiring: consumed once per boot by BOTH the device task and
 * the host synchronous path (host tests drive events through the
 * mock event loop right after health_task_start returns). */
static void health_task_body(void)
{
    /* Fresh boot = fresh window (explicit reset also keeps the
     * host-test process shared across tests honest). */
    health_window_reset(&s_win);
    s_window_us  = health_window_us_from_config();
    s_threshold  = (uint32_t)CONFIG_FIRMWARE_SOFT_RECOVERY_FAILS;
    s_recovering = false;

    /* REGISTER-not-call (led.h:132 / boot.c:195-196 precedent): the
     * LED driver owns the one-shot; it invokes the cb after the 3 s
     * SOFT_RECOVERY window. */
    (void)led_on_recovery_complete(health_recovery_complete_cb);

    /* Event-driven observation only — NEVER a polling sweep. */
    (void)wifi_event_subscribe(WIFI_EVT_STA_DISCONNECTED,
                               on_sta_disconnected, NULL);
    (void)wifi_event_subscribe(WIFI_EVT_STA_GOT_IP,
                               on_got_ip, NULL);
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
#else
    /* Host: run the wiring synchronously so host tests can drive
     * events through the mock loop immediately after start returns.
     * Device parity: health_task_entry runs the same body. */
    health_task_body();
#endif
    return ESP_OK;
}

#ifndef UNITY_HOST_BUILD
/* Device task body: wire subscriptions + recovery-cb registration,
 * then suspend. All evaluation is event-driven from the IDF event
 * loop; the suspended task stays observable via `ps` (FW-23). */
static void health_task_entry(void *arg)
{
    (void)arg;
    health_task_body();
    vTaskSuspend(NULL);
}
#endif
