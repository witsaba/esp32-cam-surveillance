/* test_wifi_recovery.c — FW-08.2 AP-reboot recovery + counter reset.
 *
 * Two scenarios cover FW-08.2 charter L750-755:
 *
 *   S1 — AP reboot is followed by reconnect.
 *     Drive WIFI_EVENT_STA_DISCONNECTED, fire the backoff timer
 *     callback, and assert esp_wifi_connect is called a second
 *     time within the 30 s cap. The mock's start_once timeout_us
 *     must be <= 30_000_000 (the cap from FW-08.1).
 *
 *   S2 — backoff counter resets on successful reconnect.
 *     Accumulate 3 consecutive failures (3x WIFI_EVENT_STA_DIS-
 *     CONNECTED + 3x mock_esp_timer_fire_callback), fire
 *     IP_EVENT_STA_GOT_IP, then 1 more WIFI_EVENT_STA_DIS-
 *     CONNECTED + fire the timer. The next backoff MUST be the
 *     2 s initial period (delay_ms(1) = 2000), NOT the 8 s that
 *     failure-4 would yield (delay_ms(4) = 16000). This proves
 *     the counter reset on GOT_IP, not on CONNECTED (per
 *     explore #3681 § Findings §10 + proposal #3682 § Decision §5).
 *
 * Both scenarios prime the standard mock surface (esp_wifi_set_*
 * returns ESP_OK, esp_netif_create_default_wifi_sta returns the
 * sentinel STA handle, esp_timer_create + start_once return OK)
 * and call wifi_init(cfg) once to wire the connect driver +
 * event handlers. The wifi component then receives the test-
 * driven WIFI/IP events through mock_esp_event_fire_handler.
 */
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_timer_link.h"
#include "mock_softap_link.h"

#include "config.h"
#include "mock_esp_wifi.h"
#include "mock_esp_netif.h"
#include "mock_esp_event.h"
#include "mock_esp_timer.h"
#include "mock_softap.h"
#include "wifi.h"
#include "wifi_event.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* Standard fixture: reset all mocks + prime returns + init wifi. */
static esp_err_t wifi_init_with_mocks(void)
{
    mock_esp_wifi_reset();
    mock_esp_netif_reset();
    mock_esp_event_reset();
    mock_esp_timer_reset();
    mock_softap_reset();

    mock_esp_wifi_init_return_set(ESP_OK);
    mock_esp_wifi_set_mode_return_set(ESP_OK);
    mock_esp_wifi_set_config_return_set(ESP_OK);
    mock_esp_wifi_start_return_set(ESP_OK);
    mock_esp_wifi_connect_return_set(ESP_OK);
    mock_esp_timer_create_set_return(ESP_OK);
    mock_esp_timer_start_once_set_return(ESP_OK);

    config_t cfg = {0};
    /* Non-empty SSID so wifi_init validates OK (FW-08.3 covers
     * the empty-SSID rejection path). */
    const char *ssid = "HomeNetwork";
    for (size_t i = 0; ssid[i] != '\0' && i < sizeof(cfg.wifi.ssid) - 1; ++i) {
        cfg.wifi.ssid[i] = ssid[i];
    }

    return wifi_init(&cfg);
}

/* Fire a STA-disconnect event via the mock. The event_data is
 * unused by the handler (the handler ignores WIFI_REASON_*). */
static void drive_sta_disconnect(void)
{
    (void)mock_esp_event_fire_handler(WIFI_EVENT,
                                       WIFI_EVENT_STA_DISCONNECTED,
                                       NULL);
}

/* Fire an IP-up event via the mock. The event_data is unused
 * (the counter-reset handler doesn't need the IP info). */
static void drive_ip_got_ip(void)
{
    (void)mock_esp_event_fire_handler(IP_EVENT,
                                       IP_EVENT_STA_GOT_IP,
                                       NULL);
}

/* Fire the backoff timer callback. wifi_init() creates the timer
 * handle as the first esp_timer handle registered, so it's
 * handle index 0. */
static void fire_backoff_timer(void)
{
    esp_timer_handle_t h = mock_esp_timer_handle_at(0);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(ESP_OK, mock_esp_timer_fire_callback(h));
}

TEST_CASE(
    "test_fw08_2_ap_reboot_reconnects_within_30s [fw-08.2][scenario-S1]",
    "[wifi][fw-08.2][recovery]")
{
    esp_err_t rc = wifi_init_with_mocks();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* After wifi_init the first esp_wifi_connect() should have
     * been issued. */
    int connect_count_after_init = mock_esp_wifi_connect_call_count();
    TEST_ASSERT_GREATER_THAN_INT(0, connect_count_after_init);

    /* Drive a STA-disconnect (AP reboot). The handler must
     * increment the counter and arm the backoff timer via
     * esp_timer_start_once. */
    drive_sta_disconnect();

    /* Fire the backoff timer — the retry callback re-issues
     * esp_wifi_connect(). */
    fire_backoff_timer();

    /* The connect call_count MUST increase by 1 (the retry). */
    int connect_count_after_retry = mock_esp_wifi_connect_call_count();
    TEST_ASSERT_GREATER_THAN_INT(connect_count_after_init,
                                  connect_count_after_retry);

    /* The backoff timer was armed with a period within the
     * 30_000 ms cap (per FW-08.1 S5). 30_000 ms = 30_000_000 us.
     * Cast to uint32_t because the host build disables Unity's
     * 64-bit support; the value comfortably fits in 32 bits. */
    uint32_t timeout_us = (uint32_t)mock_esp_timer_last_period_us_oneshot();
    TEST_ASSERT_LESS_OR_EQUAL_UINT(30000000u, timeout_us);
}

TEST_CASE(
    "test_fw08_2_counter_resets_on_ip_up [fw-08.2][scenario-S2]",
    "[wifi][fw-08.2][counter-reset]")
{
    esp_err_t rc = wifi_init_with_mocks();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Accumulate 3 consecutive failures — counter is now 3. */
    drive_sta_disconnect();
    fire_backoff_timer();
    drive_sta_disconnect();
    fire_backoff_timer();
    drive_sta_disconnect();
    fire_backoff_timer();

    /* Fire IP_EVENT_STA_GOT_IP — the handler MUST reset the
     * counter to 0. */
    drive_ip_got_ip();

    /* One more failure → counter is 1 (reset + incremented).
     * The backoff MUST be 2000 ms (= 2_000_000 us), NOT 16000 ms
     * (= 16_000_000 us) that failure-4 would have produced. */
    drive_sta_disconnect();
    fire_backoff_timer();

    uint32_t timeout_us = (uint32_t)mock_esp_timer_last_period_us_oneshot();
    TEST_ASSERT_EQUAL_UINT(2000000u, timeout_us);
}
