/* test_softap_sta_listener.c — host tests for the FW-05.5
 * always-on /whoami listener on the STA interface.
 *
 * Scenarios:
 *   - test_fw05_5_install_subscribes_both_events: calling
 *     softap_sta_listener_install() registers two wifi_event_subscribe
 *     handlers (GOT_IP + DISCONNECTED) via the mock capture table.
 *   - test_fw05_5_ip_up_starts_httpd: firing IP_EVENT_STA_GOT_IP via
 *     the mock causes httpd_start + httpd_register_uri_handler
 *     (for /whoami). softap_sta_listener_is_active() returns true.
 *   - test_fw05_5_disconnect_stops_httpd: firing WIFI_EVT_STA_DISCONNECTED
 *     (after IP-up) calls httpd_stop. is_active() returns false.
 *   - test_fw05_5_idempotent_ip_up: a duplicate IP_EVENT_STA_GOT_IP
 *     does NOT start a second httpd instance (the listener guards
 *     via the s_sta_httpd != NULL check).
 *
 * The softAP httpd (FW-05) is separate from this listener — the
 * two co-exist as distinct httpd instances. Tests below only
 * touch the STA listener; the FW-05 softAP tests in
 * test_softap_whoami.c + test_softap_provision.c cover the
 * softAP httpd in isolation.
 */
#include "unity.h"
#include "unity_host_test_runner.h"

#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"     /* for IP_EVENT base */
#include "esp_wifi.h"      /* for WIFI_EVENT base */

#include "mock_esp_event.h"
#include "mock_esp_event_link.h"   /* IP_EVENT_STA_GOT_IP / WIFI_EVENT_STA_DISCONNECTED */
#include "mock_http_server.h"
#include "mock_softap.h"
#include "mock_config.h"

#include "softap.h"
#include "wifi_event.h"

extern void softap_sta_listener_reset_for_test(void);
extern bool softap_sta_listener_is_active(void);

/* ---------- helpers ---------- */

static void reset_mocks(void)
{
    mock_httpd_reset();
    mock_esp_event_reset();
    mock_softap_reset();
    mock_config_reset();
    softap_sta_listener_reset_for_test();
}

/* ---------- scenarios ---------- */

TEST_CASE(
    "test_fw05_5_install_subscribes_both_events [fw-05.5][install][scenario-S1]",
    "[softap-sta][fw-05.5][install]")
{
    reset_mocks();

    /* Baseline: zero handler registrations before install. */
    TEST_ASSERT_EQUAL_INT(
        0, mock_esp_event_handler_instance_register_call_count());

    esp_err_t rc = softap_sta_listener_install();

    /* Two wifi_event_subscribe calls — GOT_IP + DISCONNECTED. Each
     * routes through esp_event_handler_instance_register on device
     * (or the mock capture table on host). The mock capture-table
     * counts registrations, not the public subscribe API, so we
     * check the indirect count: 2 register calls inside install. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
    TEST_ASSERT_EQUAL_INT(
        2, mock_esp_event_handler_instance_register_call_count());

    /* The listener is NOT yet active — install only subscribes,
     * the httpd starts on the first IP_EVENT_STA_GOT_IP. */
    TEST_ASSERT_FALSE(softap_sta_listener_is_active());
}

TEST_CASE("test_fw05_5_ip_up_starts_httpd [fw-05.5][ip-up][scenario-S1]", "[softap-sta][fw-05.5][ip-up]")
{
    reset_mocks();

    esp_err_t rc = softap_sta_listener_install();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    int start_before  = mock_httpd_start_call_count();
    int register_before = mock_httpd_register_uri_handler_call_count();

    /* Simulate the IP_EVENT_STA_GOT_IP dispatch. The mock's
     * fire_handler now invokes ALL matching subscribers (see
     * mock_esp_event.c multi-handler fix). The FW-08 wifi
     * component also subscribes to IP_EVENT_STA_GOT_IP; its
     * handler is a real function that calls esp_wifi_stop +
     * softap_stop on device, but on host it just does whatever
     * the wifi_event.c on_sta_got_ip_handler does in the host
     * build (no-op stub under UNITY_HOST_BUILD when no stub
     * gate is set). Our listener fires regardless. */
    mock_esp_event_fire_handler(IP_EVENT,
                               IP_EVENT_STA_GOT_IP,
                               NULL);

    /* Exactly one new httpd_start invocation. */
    TEST_ASSERT_EQUAL_INT(1, mock_httpd_start_call_count() - start_before);

    /* Two new URI registrations from THIS module — /whoami
     * (FW-05.5) + /snapshot (diagnostic frame endpoint). The FW-16
     * /cams WebSocket endpoint is attached by the ws component's
     * own GOT_IP subscription, not by this listener. */
    TEST_ASSERT_EQUAL_INT(
        2, mock_httpd_register_uri_handler_call_count() - register_before);

    /* The listener reports active. */
    TEST_ASSERT_TRUE(softap_sta_listener_is_active());

    /* The most-recently registered URI is /snapshot. */
    const char *uri = NULL;
    int          method = -1;
    mock_httpd_last_registered_uri(&uri, &method);
    TEST_ASSERT_NOT_NULL(uri);
    TEST_ASSERT_EQUAL_STRING("/snapshot", uri);
    TEST_ASSERT_EQUAL_INT(HTTP_GET, method);
}

TEST_CASE("test_fw05_5_disconnect_stops_httpd [fw-05.5][disconnect][scenario-S2]", "[softap-sta][fw-05.5][disconnect]")
{
    reset_mocks();

    esp_err_t rc = softap_sta_listener_install();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Bring the listener up. */
    (void)mock_esp_event_fire_handler(IP_EVENT,
                                      IP_EVENT_STA_GOT_IP,
                                      NULL);
    TEST_ASSERT_TRUE(softap_sta_listener_is_active());

    int stop_before = mock_httpd_stop_call_count();

    /* Now simulate a STA disconnect — WIFI_EVENT base + id 5
     * (WIFI_EVENT_STA_DISCONNECTED per IDF v5.5.3). The wifi
     * component's wifi_event_subscribe maps WIFI_EVT_STA_
     * DISCONNECTED to (WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED)
     * in wifi.c::wifi_event_subscribe(). Mirror that mapping
     * here so the mock fires the right subscriber. */
    (void)mock_esp_event_fire_handler(WIFI_EVENT,
                                      5,  /* WIFI_EVENT_STA_DISCONNECTED */
                                      NULL);

    /* httpd_stop fired exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_httpd_stop_call_count() - stop_before);

    /* Listener reports inactive. */
    TEST_ASSERT_FALSE(softap_sta_listener_is_active());
}

TEST_CASE("test_fw05_5_idempotent_ip_up [fw-05.5][idempotent][scenario-S3]", "[softap-sta][fw-05.5][idempotent]")
{
    reset_mocks();

    esp_err_t rc = softap_sta_listener_install();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* First IP-up — listener starts. */
    (void)mock_esp_event_fire_handler(IP_EVENT,
                                      IP_EVENT_STA_GOT_IP,
                                      NULL);
    TEST_ASSERT_TRUE(softap_sta_listener_is_active());
    int starts_after_first = mock_httpd_start_call_count();

    /* Second IP-up (e.g., from a wifi reconnect that re-issues
     * IP_EVENT_STA_GOT_IP). The listener should guard via the
     * s_sta_httpd != NULL check and NOT start a second httpd. */
    (void)mock_esp_event_fire_handler(IP_EVENT,
                                      IP_EVENT_STA_GOT_IP,
                                      NULL);
    TEST_ASSERT_EQUAL_INT(
        starts_after_first, mock_httpd_start_call_count());

    /* Listener still active. */
    TEST_ASSERT_TRUE(softap_sta_listener_is_active());
}
