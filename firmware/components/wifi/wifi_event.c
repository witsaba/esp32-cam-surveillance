/* wifi_event.c — Wi-Fi event handlers + counter (FW-08).
 *
 * The wifi component's event subscription seam. Owns:
 *   - s_consecutive_failures (module-static; reset on IP_EVENT
 *     _STA_GOT_IP per explore #3681 § Findings §10).
 *   - s_backoff_handle (set by wifi_init via the
 *     wifi_event_install_retry_cb seam — single-component
 *     multi-TU pattern).
 *   - on_sta_disconnected_handler (FW-08.1 + FW-08.2): increment
 *     counter + arm esp_timer_start_once + led_set_state(BACKOFF).
 *   - on_sta_got_ip_handler (FW-08.2 + FW-08.4 + FW-08.6): reset
 *     counter + esp_timer_stop + led_set_state(CONNECTED_IDLE) +
 *     softap teardown (Kconfig-gated; F-08.4).
 *   - retry_cb (esp_timer one-shot): re-issues esp_wifi_connect.
 */
#include "wifi_event.h"
#include "wifi.h"

#include "esp_err.h"
#include "led.h"
#include "softap.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_wifi_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_timer_link.h"
#include "mock_softap_link.h"
#else
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#endif

/* Module-static state. Mirrors the FW-06 led.c `volatile
 * g_state` idiom: single-uint write is atomic on Xtensa LX6. */
static uint32_t          s_consecutive_failures = 0;
static esp_timer_handle_t s_backoff_handle      = NULL;

/* Forward declaration — installed by wifi_init() via the seam
 * below. Defined at the bottom of this TU. */
static void retry_cb(void *arg);

void wifi_event_install_retry_cb(esp_timer_handle_t h,
                                     esp_timer_cb_t cb)
{
    (void)cb;
    s_backoff_handle = h;
}

/* Public symbol referenced by wifi.c::wifi_init() so the timer
 * creation sees a non-NULL callback. Body defined below. */
void wifi_event_retry_cb(void *arg);

void on_sta_disconnected_handler(void *arg,
                                    const char *base,
                                    int32_t id,
                                    void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    /* FW-08.1 + FW-08.2: bump counter, arm backoff timer with
     * the new delay. The retry cb re-issues esp_wifi_connect
     * when the timer fires. */
    s_consecutive_failures++;
    uint32_t delay_ms = wifi_backoff_delay_ms(s_consecutive_failures);

    /* Convert ms -> us for esp_timer_start_once. */
    if (s_backoff_handle) {
        (void)esp_timer_start_once(s_backoff_handle,
                                    (uint64_t)delay_ms * 1000ull);
    }
    (void)led_set_state(LED_STATE_RECONNECT_BACKOFF);
}

void on_sta_got_ip_handler(void *arg,
                            const char *base,
                            int32_t id,
                            void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    /* FW-08.2 — reset counter on IP_EVENT_STA_GOT_IP (NOT on
     * WIFI_EVENT_STA_CONNECTED; resetting on association would
     * break the DHCP-failure backoff invariant). Also stop the
     * backoff timer so the retry cb can't fire spuriously. */
    s_consecutive_failures = 0;
    if (s_backoff_handle) {
        (void)esp_timer_stop(s_backoff_handle);
    }
    (void)led_set_state(LED_STATE_CONNECTED_IDLE);

    /* FW-08.4 — softAP teardown (Kconfig-gated). Full body
     * lands in T-08-E. */
#if defined(CONFIG_FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT) && \
    CONFIG_FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT
    (void)wifi_stop();
#endif
}

/* One-shot timer callback. Re-issues esp_wifi_connect() when
 * the backoff window elapses. The IDF event loop continues to
 * drive the retry state machine on WIFI_EVENT_STA_DISCONNECTED
 * + GOT_IP. Public symbol (not static) because wifi.c's
 * esp_timer_create_args must point at a non-NULL callback
 * (mock_esp_timer.c:121 returns ESP_ERR_INVALID_ARG for NULL
 * callback). */
void wifi_event_retry_cb(void *arg)
{
    (void)arg;
    (void)esp_wifi_connect();
}
