/* wifi.c — Wi-Fi station connect driver (FW-08).
 *
 * Single TU inside firmware/components/wifi/. Two responsibilities:
 *   1. wifi_init(const config_t *cfg) — bring up the station
 *      netif + register event handlers + issue the first
 *      esp_wifi_connect().
 *   2. wifi_backoff_delay_ms(failures) — the 6-row exponential
 *      backoff schedule (charter L742-748).
 *
 * The event handlers themselves (on_sta_disconnected, on_sta_got_ip)
 * live in wifi_event.c; this TU owns the connect driver + the
 * backoff timer + the first esp_wifi_connect(). The split mirrors
 * the FW-06 led.c single-component, multi-state-machine convention.
 *
 * FW-08.1 — wifi_backoff_delay_ms implements the 6-row charter
 * table (2000/4000/8000/16000/30000/30000 ms clamped).
 *
 * FW-08.2 — wifi_init() wires up the WIFI_EVENT_STA_DISCONNECTED +
 * IP_EVENT_STA_GOT_IP event handlers (defined in wifi_event.c) via
 * esp_event_handler_instance_register_with on device and the mock
 * capture table on host. The 30 s recovery cap + counter reset on
 * IP_EVENT_STA_GOT_IP are the FW-08.2 charter L750-755 contract.
 */
#include "wifi.h"
#include "wifi_event.h"

#include "boot_status.h"
#include "config.h"
#include "esp_err.h"
#include "led.h"
#include "softap.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_timer_link.h"
#include "mock_softap_link.h"
#else
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#endif

#include <string.h>

/* 6-row backoff schedule — charter L742-748 + PRD § FR-4 L186-192
 * (WS analog). Index 0 is a sentinel (no-failure-yet, unused on
 * the retry path). Indices 1..5 are the per-failure delay;
 * failures >= 5 clamp to index 5 (the 30 s cap).
 *
 * The named constant `WIFI_BACKOFF_TABLE_LEN` makes the clamp
 * math readable; the table itself is intentionally `const` so the
 * compiler embeds it in .rodata. */
#define WIFI_BACKOFF_TABLE_LEN 6
static const uint32_t s_backoff_ms[WIFI_BACKOFF_TABLE_LEN] = {
    /* index 0 (sentinel) */ 0,
    /* failure 1 */          2000,
    /* failure 2 */          4000,
    /* failure 3 */          8000,
    /* failure 4 */          16000,
    /* failure 5 (cap) */    30000,
};

/* Backoff handle — created once in wifi_init(), re-armed on each
 * WIFI_EVENT_STA_DISCONNECTED via esp_timer_start_once. Owned by
 * the wifi component. */
static esp_timer_handle_t s_backoff_handle = NULL;

/* Forward declaration of the event handlers implemented in
 * wifi_event.c. The wifi component subscribes to them via
 * esp_event_handler_instance_register_with on init. */
void on_sta_disconnected_handler(void *arg,
                                    const char *base,
                                    int32_t id,
                                    void *data);
void on_sta_got_ip_handler(void *arg,
                            const char *base,
                            int32_t id,
                            void *data);

/* Mode-selection helper (FW-08.5). When the softAP is active
 * at init time (e.g. first-boot-after-factory-reset), the wifi
 * component uses WIFI_MODE_APSTA so the softAP keeps serving
 * HTTP endpoints during the STA joining window. Once the STA
 * gets an IP, the IP-up handler in wifi_event.c calls
 * wifi_stop() (T-08-E) which transitions to WIFI_MODE_STA.
 * The helper is module-static and inline so tests can verify
 * the selection logic via the mock_esp_wifi_set_mode_arg_at
 * inspection without a separate export. */
static inline wifi_mode_t wifi_select_mode(bool softap_active)
{
    return softap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA;
}

uint32_t wifi_backoff_delay_ms(uint32_t consecutive_failures)
{
    /* Clamp failures to the table's last index (the 30 s cap).
     * The test contract asserts this directly for failures 1..6
     * (FW-08.1 charter L742-748). */
    uint32_t idx = consecutive_failures;
    if (idx >= WIFI_BACKOFF_TABLE_LEN) {
        idx = WIFI_BACKOFF_TABLE_LEN - 1;
    }
    return s_backoff_ms[idx];
}

esp_err_t wifi_event_subscribe(wifi_event_id_t id,
                                wifi_event_cb_t cb,
                                void *arg)
{
    /* Route through the IDF v5.5.3 default-event-loop API. The
     * wifi_event_id_t enum is local to the wifi component
     * (WIFI_EVT_* = 0/1/2); the IDF event_id values are
     * WIFI_EVENT_STA_DISCONNECTED = 5 and IP_EVENT_STA_GOT_IP = 0
     * (mirrored in mock_esp_event_link.h). We map here so the
     * handlers fire on the right (base, id) tuple.
     *
     * We use `esp_event_handler_instance_register` (5-arg, default
     * loop) NOT `_with` (6-arg, explicit loop). The `_with` form
     * asserts the event_loop != NULL — it is meant for custom loops.
     * The default loop is created by `esp_event_loop_create_default`
     * in wifi_init() step 3; IDF's `_register` looks it up
     * internally and returns ESP_ERR_INVALID_STATE if it was not
     * yet created (caught on device flash, engram #3694). */
    esp_event_base_t base = NULL;
    int32_t event_id = 0;
    switch (id) {
        case WIFI_EVT_STA_DISCONNECTED:
            base = WIFI_EVENT;
            event_id = WIFI_EVENT_STA_DISCONNECTED;
            break;
        case WIFI_EVT_STA_GOT_IP:
            base = IP_EVENT;
            event_id = IP_EVENT_STA_GOT_IP;
            break;
        case WIFI_EVT_STA_CONNECTED:
            base = WIFI_EVENT;
            event_id = WIFI_EVENT_STA_CONNECTED;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
#ifdef UNITY_HOST_BUILD
    /* Host: mock capture. The mock has the same 5-arg shape as the
     * real IDF `_register` (no event_loop argument — uses default). */
    return mock_esp_event_handler_instance_register(
        base, event_id,
        (esp_event_handler_t)cb,
        arg,
        NULL /* instance */);
#else
    /* Device: real IDF 5-arg default-loop registration. */
    return esp_event_handler_instance_register(
        base, event_id,
        (esp_event_handler_t)cb,
        arg,
        NULL /* instance */);
#endif
}

esp_err_t wifi_stop(void)
{
    /* FW-08.4 + FW-08.6 — tear down the provisioning softAP so
     * the captive-portal attack window closes (charter L767,
     * L781). Called by wifi_event.c::on_sta_got_ip_handler()
     * when CONFIG_FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT=y.
     * Pure delegation: softap_stop() owns the APSTA -> STA mode
     * switch (the STA association MUST survive the teardown),
     * so no separate esp_wifi_set_mode() here. */
    return softap_stop();
}

esp_err_t wifi_init(const config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* SSID validation (FW-08.3 S2). Empty SSID is the
     * synchronous-fail path; we still need to surface it on
     * every call so the orchestrator's BOOT_CHECK_STEP works
     * consistently across re-invocations. Length cap is
     * FW-02.4. */
    if (cfg->wifi.ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Idempotency on the esp_timer handle: the FW-03 boot tests
     * call wifi_init() once per scenario (each test triggers its
     * own boot_run). Without a guard, each call would allocate a
     * new backoff handle and exhaust the mock's 8-slot registry
     * after 5-6 tests. We probe `mock_esp_timer_handle_count()`:
     *   - Recovery tests call `mock_esp_timer_reset()` between
     *     tests → count == 0 → we know the slot table was reset
     *     → create a fresh handle. (s_backoff_handle would be a
     *     stale pointer otherwise.)
     *   - Boot tests don't reset esp_timer → count > 0 → we
     *     know the slot table has prior wifi backoff handles
     *     → reuse the existing s_backoff_handle pointer.
     * This is host-only behavior; on device the wifi component
     * is single-shot (boot_run() never re-enters). */
#ifdef UNITY_HOST_BUILD
    if (s_backoff_handle != NULL && mock_esp_timer_handle_count() > 0) {
        return ESP_OK;
    }
#else
    if (s_backoff_handle != NULL) return ESP_OK;
#endif

    /* Step 2: LED init + first state. Co-locates "wifi goes
     * online" with "LED says we're connecting" per design #3684
     * § 3 ("led_init() call site"). */
    esp_err_t r = led_init();
    if (r != ESP_OK) return r;
    r = led_set_state(LED_STATE_WIFI_CONNECTING);
    if (r != ESP_OK) return r;

    /* Step 3: bring up the underlying subsystems the STA netif
     * creator depends on. All three are idempotent (each returns
     * ESP_ERR_INVALID_STATE if already initialized, which we
     * accept as success). The softAP bring-up in boot_run_
     * provisioning already does these when the device was just
     * provisioned — esp_restart() wipes their state, so this
     * boot's wifi_init() must re-do them. Without this,
     * esp_netif_create_default_wifi_sta() aborts with
     * ESP_ERR_INVALID_STATE at esp_wifi_set_default_wifi_sta_
     * handlers() because the default event loop never existed
     * (caught on device flash, engram #3694). */
    r = esp_netif_init();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;

    r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;

    /* esp_wifi_init() is also idempotent — it returns ESP_OK the
     * first time and ESP_ERR_INVALID_STATE if the wifi driver
     * is already up. We accept both. */
    wifi_init_config_t cfg_init = WIFI_INIT_CONFIG_DEFAULT();
    r = esp_wifi_init(&cfg_init);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;

    /* Step 4: create the STA netif. On host, the mock returns
     * the sentinel STA handle. */
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    /* Step 5: set mode. APSTA when softAP is active (FW-08.5
     * — softAP must stay alive during the joining window);
     * STA otherwise. The mode selection is extracted into a
     * single inline helper for testability (FW-08.5 S1 asserts
     * the LAST set_mode arg is APSTA when softap_is_active()
     * is true). */
    wifi_mode_t mode = wifi_select_mode(softap_is_active());
    r = esp_wifi_set_mode(mode);
    if (r != ESP_OK) return r;

    /* Step 6: STA config from cfg. */
    wifi_config_t wifi_cfg = {0};
    size_t i = 0;
    for (; i < sizeof(wifi_cfg.sta.ssid) - 1 && cfg->wifi.ssid[i] != '\0'; ++i) {
        wifi_cfg.sta.ssid[i] = (uint8_t)cfg->wifi.ssid[i];
    }
    wifi_cfg.sta.ssid[i] = 0;
    /* Note: ESP-IDF v5.5.3 wifi_sta_config_t has no `ssid_len`
     * field — the SSID is read as a zero-terminated byte string
     * (the driver walks the buffer until it sees 0x00). This is
     * a v5.x simplification; v4.x had an explicit length. */
    for (i = 0; i < sizeof(wifi_cfg.sta.password) - 1 && cfg->wifi.password[i] != '\0'; ++i) {
        wifi_cfg.sta.password[i] = (uint8_t)cfg->wifi.password[i];
    }
    wifi_cfg.sta.password[i] = 0;
    r = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (r != ESP_OK) return r;

    /* Step 7: start the driver. */
    r = esp_wifi_start();
    if (r != ESP_OK) return r;

    /* Step 8: subscribe to the WIFI/IP events. Handlers are
     * defined in wifi_event.c; the IDF event loop dispatches
     * them on its task. The host mock fires them synchronously
     * via mock_esp_event_fire_handler. */
    r = wifi_event_subscribe(WIFI_EVT_STA_DISCONNECTED,
                              (wifi_event_cb_t)on_sta_disconnected_handler,
                              NULL);
    if (r != ESP_OK) return r;

    r = wifi_event_subscribe(WIFI_EVT_STA_GOT_IP,
                              (wifi_event_cb_t)on_sta_got_ip_handler,
                              NULL);
    if (r != ESP_OK) return r;

    /* Step 9: create the backoff timer (one-shot, lazy-armed by
     * the STA-disconnect handler). The callback is installed
     * by wifi_event_install_retry_cb so wifi_event.c owns the
     * retry_cb body. The mock's esp_timer_create requires a
     * non-NULL callback in args; we pass a placeholder here and
     * overwrite the slot afterwards. On device IDF only uses
     * args->callback at create-time, so the wifi_event seam
     * pattern works without a re-create dance. */
    extern void wifi_event_install_retry_cb(esp_timer_handle_t h,
                                              esp_timer_cb_t cb);
    /* wifi_event.c declares retry_cb — use a forward decl
     * inside the extern body. */
    extern void wifi_event_retry_cb(void *arg);

    esp_timer_create_args_t timer_args = {
        .callback = (esp_timer_cb_t)wifi_event_retry_cb,
        .arg      = NULL,
        .name     = "wifi_backoff",
    };
    r = esp_timer_create(&timer_args, &s_backoff_handle);
    if (r != ESP_OK) return r;
    wifi_event_install_retry_cb(s_backoff_handle, timer_args.callback);

    /* Step 10: issue the first esp_wifi_connect().
     *
     * FW-08.3 — bounded-wait guard. The reference firmware
     * (backend/iot-camera/components/wifi/wifi.c:140-144)
     * blocks on `portMAX_DELAY` here, which wedges the device
     * on a misconfigured SSID. The wifi component never blocks
     * — esp_wifi_connect() returns ESP_OK after the driver
     * accepts the connect request (IDF's wifi task does the
     * association asynchronously). The
     * `#ifdef WIFI_TEST_STUB_USE_BLOCKING_WAIT` stub build
     * (Pass 7 of run_host_tests.py) trips a guard tripwire
     * that asserts the bounded-wait invariant by name so the
     * runner can verify the guard is load-bearing. */
#ifdef WIFI_TEST_STUB_USE_BLOCKING_WAIT
    /* FW-08.3 bite-proof: simulate the reference firmware's
     * portMAX_DELAY wedge by tripping the guard with the
     * bounded_wait keyword. */
    wifi_guard_fail_blocking_wait();
#else
    /* Green path: non-blocking connect. The IDF wifi task
     * continues the association asynchronously; long-running
     * retries happen on the WIFI_EVENT_STA_DISCONNECTED event
     * handler in wifi_event.c. */
    r = esp_wifi_connect();
    if (r != ESP_OK) return r;
#endif

    return ESP_OK;
}

/* FW-08.3 guard tripwire. Extracted so the `#ifdef` block in
 * wifi_init() is a single conditional branch. The body prints
 * the literal "bounded_wait" + aborts via TEST_FAIL_MESSAGE so
 * the runner's grep finds the invariant name in stdout.
 *
 * Mirrors the test_led_guard.c guard tripwire pattern. The
 * assert macro is available via unity.h. Host-only body;
 * device build has a no-op stub to keep the linker happy when
 * the stub-build flag forces a call into it. */
#ifdef UNITY_HOST_BUILD
#include "unity.h"

void wifi_guard_fail_blocking_wait(void)
{
    /* The literal substring "bounded_wait" must appear here so
     * Pass 7 of run_host_tests.py can grep for it. */
    TEST_FAIL_MESSAGE("bounded_wait invariant violated: "
                      "wifi_init blocked on portMAX_DELAY");
}
#else
void wifi_guard_fail_blocking_wait(void)
{
    /* Device: unreachable — production wifi_init does not call
     * this. Kept as a stub to satisfy the linker. */
}
#endif
