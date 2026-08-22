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
 * backoff timer + the consecutive_failures counter is reached via
 * the wifi_event.c seam. The split mirrors the FW-06 led.c single-
 * component, multi-state-machine convention.
 *
 * Stub body for T-08-A (build infra): both functions return
 * ESP_OK/0 to confirm the wifi component compiles + links. T-08-B
 * fleshes out wifi_init + wifi_backoff_delay_ms. T-08-C adds the
 * event handlers + counter. T-08-D adds the FW-08.3 bounded-wait
 * guard. T-08-F adds the WIFI_MODE_APSTA selection. T-08-G adds
 * the FW-08.6 teardown guard.
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
#else
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#endif

/* 6-row backoff schedule — charter L742-748. Index 0 is a
 * sentinel (no-failure-yet, unused on the retry path). Indices 1..5
 * are the per-failure delay; failures >= 5 clamp to index 5. */
static const uint32_t s_backoff_ms[6] = { 0, 2000, 4000, 8000, 16000, 30000 };

/* Backoff handle — created once in wifi_init(), re-armed on each
 * WIFI_EVENT_STA_DISCONNECTED via esp_timer_start_once. Owned by
 * the wifi component. */
static esp_timer_handle_t s_backoff_handle = NULL;

uint32_t wifi_backoff_delay_ms(uint32_t consecutive_failures)
{
    /* Clamp failures to the table's last index (cap). The test
     * contract asserts this directly for failures 1..6. */
    uint32_t idx = consecutive_failures;
    if (idx >= sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0])) {
        idx = (sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0])) - 1;
    }
    return s_backoff_ms[idx];
}

esp_err_t wifi_event_subscribe(wifi_event_id_t id,
                                wifi_event_cb_t cb,
                                void *arg)
{
    (void)id;
    (void)cb;
    (void)arg;
    /* Fleshed out in T-08-C. The stub returns ESP_OK so the wifi
     * component compiles + links under T-08-A (build infra). */
    return ESP_OK;
}

esp_err_t wifi_stop(void)
{
    /* Fleshed out in T-08-E. */
    return ESP_OK;
}

esp_err_t wifi_init(const config_t *cfg)
{
    (void)cfg;
    /* Fleshed out in T-08-B + T-08-C + T-08-D. T-08-A only verifies
     * the wifi component compiles + links. */
    return ESP_OK;
}
