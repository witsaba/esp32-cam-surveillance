/* wifi.h — public API for the Wi-Fi station connect driver (FW-08).
 *
 * Two deliverables in one flow (per charter L734):
 *   1. wifi_init(const config_t *) — connects the station to the
 *      configured SSID with exponential backoff on failure.
 *   2. wifi_event_subscribe + wifi_stop — event subscription seam
 *      + idempotent teardown helper called by the IP-up handler.
 *
 * Replaces the stub body at firmware/components/boot/stub_inits.c:27-35;
 * boot.c:151's call site is unchanged.
 *
 * Host builds (`UNITY_HOST_BUILD` defined) include the mock link
 * headers BEFORE the IDF Wi-Fi / netif / event headers. The mocks
 * swap every production call for the host-friendly equivalent.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "boot_status.h"
#include "config.h"
#include "esp_err.h"
#include "wifi_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the wifi station connect driver.
 *
 * Green path: returns ESP_OK after esp_wifi_start() + the first
 * esp_wifi_connect() call has been issued and the WIFI/IP event
 * handlers are registered. Long-running backoff retries happen on
 * the IDF event task, NOT on the boot thread.
 *
 * Returns ESP_ERR_INVALID_ARG synchronously if cfg->wifi.ssid is
 * empty (FW-02.4 length cap). Returns the failing esp_err_t if any
 * IDF init step fails (BOOT_STEP_WIFI_INIT surfaces it via boot.c).
 *
 * Side effects on green path:
 *   - led_init() + led_set_state(LED_STATE_WIFI_CONNECTING)
 *   - esp_netif_create_default_wifi_sta()
 *   - esp_wifi_set_mode(WIFI_MODE_APSTA) if softap_is_active(),
 *     else WIFI_MODE_STA
 *   - esp_wifi_set_config(WIFI_IF_STA, &cfg->wifi)
 *   - esp_wifi_start()
 *   - wifi_event_subscribe for STA_DISCONNECTED + STA_GOT_IP
 *   - esp_timer_create for the backoff handle (one-shot, lazy-armed)
 *   - esp_wifi_connect() (first call) */
esp_err_t wifi_init(const config_t *cfg);

/* Event-subscription seam. Routes through
 * esp_event_handler_instance_register_with on device and the mock
 * capture table on host. The handler is invoked as
 * handler(arg, base, id, event_data). */
esp_err_t wifi_event_subscribe(wifi_event_id_t id,
                                wifi_event_cb_t cb,
                                void *arg);

/* Tear-down helper. Called by wifi_event.c::ip_up_handler() under
 * CONFIG_FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT=y. Idempotent —
 * safe to call when already torn down. */
esp_err_t wifi_stop(void);

/* Backoff schedule (mirrors the 6-row charter L742-748 table):
 *   failure 1 -> 2000 ms
 *   failure 2 -> 4000 ms
 *   failure 3 -> 8000 ms
 *   failure 4 -> 16000 ms
 *   failure 5 -> 30000 ms (cap)
 *   failure 6+ -> 30000 ms (cap holds)
 *
 * Public for testability + FW-15 future consumption. */
uint32_t wifi_backoff_delay_ms(uint32_t consecutive_failures);

/* FW-08.3 — guard tripwire. When the build defines
 * `-DWIFI_TEST_STUB_USE_BLOCKING_WAIT=1`, wifi_init() short-
 * circuits its first esp_wifi_connect() branch into a call to
 * this function, which prints the literal "bounded_wait" and
 * aborts via TEST_ASSERT_MESSAGE. Pass 7 of run_host_tests.py
 * greps for the literal to confirm the guard is load-bearing.
 *
 * Not part of the production API — declared here only so the
 * wifi.c wifi_init() body can call it without a forward
 * declaration. */
void wifi_guard_fail_blocking_wait(void);

#ifdef __cplusplus
}
#endif
