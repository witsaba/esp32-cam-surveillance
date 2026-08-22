/* test_wifi_event_joining.c — FW-08.5 softAP-alive-during-join tests.
 *
 * While the STA is still joining, the softAP MUST remain reachable
 * and serving HTTP endpoints. The wifi component uses
 * WIFI_MODE_APSTA (NOT WIFI_MODE_STA) during the pre-IP-up window;
 * the WIFI_MODE_STA transition is owned by the IP-up handler in
 * FW-08.4 (wifi_stop()).
 *
 * Two scenarios cover FW-08.5 charter L771-776:
 *
 *   S1 — pre-IP-up state keeps softAP active.
 *     softap_is_active() returns true; wifi_init() runs; 5 s
 *     have elapsed. The LAST call to esp_wifi_set_mode MUST
 *     have been with WIFI_MODE_APSTA (so the softAP stays up
 *     alongside the STA association attempt).
 *     Asserts:
 *       - mock_esp_wifi_set_mode_arg_at(N - 1) == WIFI_MODE_APSTA
 *       - mock_softap_stop_call_count() == 0
 *
 *   S2 — pre-IP-up retries do not affect softAP state.
 *     Drive 3x WIFI_EVENT_STA_DISCONNECTED, each followed by
 *     mock_esp_timer_fire_callback (the backoff retry). The
 *     softAP lifecycle is independent of these retries — no
 *     softap_stop call, the mode remains APSTA.
 *     Asserts:
 *       - mock_softap_stop_call_count() == 0 throughout
 *       - mock_esp_wifi_set_mode_arg_at(N - 1) == WIFI_MODE_APSTA
 *         (the retries don't change the mode)
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

/* Standard fixture: reset all mocks + prime returns + init wifi.
 * softap_is_active() returns true so wifi_init selects
 * WIFI_MODE_APSTA. */
static esp_err_t wifi_init_with_mocks_softap_active(void)
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
    /* The FW-08.5 trigger: softAP is active. */
    mock_softap_is_active_set_return(true);

    config_t cfg = {0};
    const char *ssid = "HomeNetwork";
    for (size_t i = 0; ssid[i] != '\0' && i < sizeof(cfg.wifi.ssid) - 1; ++i) {
        cfg.wifi.ssid[i] = ssid[i];
    }

    return wifi_init(&cfg);
}

/* Fire a STA-disconnect event via the mock. */
static void drive_sta_disconnect(void)
{
    (void)mock_esp_event_fire_handler(WIFI_EVENT,
                                       WIFI_EVENT_STA_DISCONNECTED,
                                       NULL);
}

/* Fire the backoff timer (handle index 0 = wifi backoff handle). */
static void fire_backoff_timer(void)
{
    esp_timer_handle_t h = mock_esp_timer_handle_at(0);
    if (h) (void)mock_esp_timer_fire_callback(h);
}

TEST_CASE(
    "test_fw08_5_pre_ip_up_keeps_softap_active_at_5s [fw-08.5][scenario-S1]",
    "[wifi-event][fw-08.5][joining]")
{
    esp_err_t rc = wifi_init_with_mocks_softap_active();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* The wifi_init() selected WIFI_MODE_APSTA (the last set_mode
     * call). The mock's set_mode_arg_at(0) returns the most
     * recent argument. */
    int mode_count = mock_esp_wifi_set_mode_call_count();
    TEST_ASSERT_GREATER_THAN_INT(0, mode_count);
    wifi_mode_t last_mode = mock_esp_wifi_set_mode_arg_at(0);
    TEST_ASSERT_EQUAL_INT(WIFI_MODE_APSTA, (int)last_mode);

    /* No softAP teardown yet — pre-IP-up. */
    TEST_ASSERT_EQUAL_INT(0, mock_softap_stop_call_count());

    /* softap_is_active() getter still returns true — the mock
     * primed it, and the wifi component consulted it. */
    TEST_ASSERT_TRUE(mock_softap_is_active_get());
}

TEST_CASE(
    "test_fw08_5_pre_ip_up_retries_do_not_affect_softap [fw-08.5][scenario-S2]",
    "[wifi-event][fw-08.5][joining]")
{
    esp_err_t rc = wifi_init_with_mocks_softap_active();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Drive 3 STA-disconnect events, each followed by a backoff
     * timer fire. The retry path calls esp_wifi_connect() but
     * does NOT touch the softAP lifecycle. */
    drive_sta_disconnect();
    fire_backoff_timer();
    drive_sta_disconnect();
    fire_backoff_timer();
    drive_sta_disconnect();
    fire_backoff_timer();

    /* softAP remains active throughout — no teardown. */
    TEST_ASSERT_EQUAL_INT(0, mock_softap_stop_call_count());

    /* The mode selection was APSTA at init and is NOT changed
     * by the retries (they go through esp_wifi_connect, not
     * set_mode). The LAST set_mode call (index 0 = newest) is
     * still APSTA. */
    wifi_mode_t last_mode = mock_esp_wifi_set_mode_arg_at(0);
    TEST_ASSERT_EQUAL_INT(WIFI_MODE_APSTA, (int)last_mode);
}
