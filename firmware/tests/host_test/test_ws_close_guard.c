/* test_ws_close_guard.c — FW-14 clean-CLOSE sleep-invariant
 * bite-proof (Pass 12).
 *
 * This file is ONLY compiled under the Pass-12 stub build
 * (-DWS_TEST_STUB_ENABLE_CLOSE_RECONNECT=1 applied to BOTH the
 * production sources and this file). The stub flag compiles the
 * latch check out of ws_event_handler.c's failure path — i.e. it
 * simulates a future regression where a clean CLOSE no longer
 * suppresses reconnect scheduling (the sleep invariant from FR-3 +
 * FR-6).
 *
 * The single bite-proof test below then FAILS, naming the violated
 * invariant with the literal "close_no_reconnect" — proving the
 * guard is load-bearing. Mirrors the Pass 5-11 patterns
 * (timer_fire / debounce / bounded_wait / teardown / no_reinit /
 * single_owner / url_no_mac).
 *
 * There is intentionally NO green-path test here under the
 * production build: the latch orderings are covered behaviorally
 * by test_ws_reconnect_backoff.c scenarios S2-S4.
 */
#include "unity.h"

#ifdef WS_TEST_STUB_ENABLE_CLOSE_RECONNECT

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <string.h>
#include <stdio.h>

#include "ws.h"
#include "ws_backoff.h"
#include "config.h"
#include "esp_err.h"

#include "mock_esp_websocket_client.h"
#include "mock_esp_timer.h"
#include "mock_log.h"
#include "mock_esp_event.h"

static config_t s_test_cfg;

TEST_CASE(
    "test_pass12_clean_close_must_not_schedule [fw-14][close-guard][bite-proof]",
    "[ws][fw-14][close-guard]")
{
    printf("bite-proof stub build entered: close_no_reconnect\n");

    /* Fresh WS session. */
    memset(&s_test_cfg, 0, sizeof(s_test_cfg));
    strncpy(s_test_cfg.wifi.ssid, "TestSSID",
            sizeof(s_test_cfg.wifi.ssid) - 1);
    s_test_cfg.wifi.ssid[sizeof(s_test_cfg.wifi.ssid) - 1] = '\0';
    ws_backoff_reset_for_test();
    ws_status_timer_reset_handle_for_test();
    mock_esp_timer_reset();
    mock_esp_websocket_client_reset_for_test();
    mock_esp_event_reset();
    ws_event_handler_reset_for_test();
    mock_log_reset();
    /* FW-16 server mode: self-wire the retained FW-14 event chain
     * against a mock client session (ws_init no longer creates
     * the outbound client nor installs handlers). */
    esp_websocket_client_handle_t h =
        mock_esp_websocket_client_init(
            &(esp_websocket_client_config_t){0});
    TEST_ASSERT_NOT_NULL(h);
    ws_handle_set(h);
    TEST_ASSERT_EQUAL(ESP_OK, ws_event_handler_install());

    /* Clean CLOSE handshake followed by the disconnect event. With
     * the production latch in place NOTHING would be scheduled;
     * under the stub build the latch check is compiled out and the
     * failure path schedules a reconnect — tripping the invariant
     * message below. */
    mock_esp_websocket_client_fire_closed(1000);
    mock_esp_websocket_client_fire_disconnected();

    if (mock_esp_websocket_client_set_reconnect_timeout_call_count() > 0 ||
        mock_esp_timer_start_once_call_count() > 0) {
        TEST_FAIL_MESSAGE("close_no_reconnect invariant violated: "
                          "a reconnect was scheduled after a clean "
                          "CLOSE (1000) — sleep-on-clean-CLOSE is "
                          "broken");
    }
}

#endif /* WS_TEST_STUB_ENABLE_CLOSE_RECONNECT */
