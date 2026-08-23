/* test_ws_reconnect_backoff.c — FW-14.1 exponential backoff schedule
 * (REQ ws-reconnect-backoff / R-19, FR-4).
 *
 * The ws_backoff module owns the reconnect loop while
 * disable_auto_reconnect stays true. On every failure event it
 * computes delay = CONFIG_FIRMWARE_WS_RECONNECT_INITIAL_MS ×
 * 2^(n−1), capped at CONFIG_FIRMWARE_WS_RECONNECT_CAP_MS, calls
 * esp_websocket_client_set_reconnect_timeout(delay) per FR-4, arms
 * the one-shot esp_timer, and logs the transition at WARN.
 *
 * FR-4 table (INITIAL=2000, CAP=30000 defaults):
 *
 *   | consecutive_failures n | delay_ms |
 *   | 1 | 2000  |
 *   | 2 | 4000  |
 *   | 3 | 8000  |
 *   | 4 | 16000 |
 *   | 5 | 30000 |  <- cap reached
 *   | 6 | 30000 |  <- cap holds
 *
 * These six tests drive the module surface directly (unit layer):
 * n consecutive ws_backoff_on_failure() calls; the LAST captured
 * setter value must equal row n's delay. The event-handler wiring
 * (DISCONNECTED/ERROR → module) is covered by the Phase-4 tests in
 * this same file.
 *
 * Convention: assertions use uint32_t casts because Unity on the
 * host build disables 64-bit support; delays fit comfortably.
 */
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <string.h>

#include "ws.h"
#include "ws_backoff.h"
#include "config.h"
#include "esp_err.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_websocket_client.h"
#include "mock_esp_timer.h"
#include "mock_log.h"
#include "mock_esp_event.h"
#endif

static config_t s_test_cfg;

static void reset_state(void)
{
    memset(&s_test_cfg, 0, sizeof(s_test_cfg));
    strncpy(s_test_cfg.wifi.ssid, "TestSSID",
            sizeof(s_test_cfg.wifi.ssid) - 1);
    s_test_cfg.wifi.ssid[sizeof(s_test_cfg.wifi.ssid) - 1] = '\0';
#ifdef UNITY_HOST_BUILD
    /* Order matters: null the module-static backoff handle BEFORE
     * clearing the timer mock slot table, so the next failure
     * re-creates the handle in the freshly-cleared registry. */
    ws_backoff_reset_for_test();
    ws_status_timer_reset_handle_for_test();
    mock_esp_timer_reset();
    mock_esp_websocket_client_reset_for_test();
    mock_esp_event_reset();
    ws_event_handler_reset_for_test();
    mock_log_reset();
    /* Bring up a real WS session so ws_backoff_on_failure sees a
     * live handle (the FR-4 setter fires against it). */
    TEST_ASSERT_EQUAL(ESP_OK, ws_init(&s_test_cfg));
#endif
}

/* Drive `n` failures and assert row-n expectations:
 * return value == captured setter value == armed one-shot timeout,
 * counter == n, and a WARN-level transition was logged. */
static void assert_backoff_row(uint32_t n, uint32_t expected_ms)
{
    size_t warn_before = mock_log_warn_count;
    int start_once_before = mock_esp_timer_start_once_call_count();

    uint32_t d = ws_backoff_on_failure();

    TEST_ASSERT_EQUAL_UINT32(expected_ms, d);
    TEST_ASSERT_EQUAL_INT((int)expected_ms,
        mock_esp_websocket_client_get_last_reconnect_timeout_ms());
    TEST_ASSERT_EQUAL_UINT32(n, ws_backoff_failure_count());
    TEST_ASSERT_EQUAL_UINT32(expected_ms, ws_backoff_current_delay_ms());
    /* One-shot armed at delay×1000 µs (exactly one new arming).
     * Host Unity disables 64-bit support; max 30_000_000 µs fits
     * in 32 bits. */
    TEST_ASSERT_EQUAL_INT(start_once_before + 1,
        mock_esp_timer_start_once_call_count());
    TEST_ASSERT_EQUAL_UINT32(expected_ms * 1000u,
        (uint32_t)mock_esp_timer_last_period_us_oneshot());
    /* FR-4 mandates a WARN-level transition log per scheduling. */
    TEST_ASSERT_TRUE(mock_log_warn_count > warn_before);
}

TEST_CASE(
    "test_fw14_1_backoff_failures_1 [fw-14.1][row-1]",
    "[ws][fw-14.1][backoff]")
{
    reset_state();
    assert_backoff_row(1, 2000u);
}

TEST_CASE(
    "test_fw14_1_backoff_failures_2 [fw-14.1][row-2]",
    "[ws][fw-14.1][backoff]")
{
    reset_state();
    assert_backoff_row(1, 2000u);
    assert_backoff_row(2, 4000u);
}

TEST_CASE(
    "test_fw14_1_backoff_failures_3 [fw-14.1][row-3]",
    "[ws][fw-14.1][backoff]")
{
    reset_state();
    assert_backoff_row(1, 2000u);
    assert_backoff_row(2, 4000u);
    assert_backoff_row(3, 8000u);
}

TEST_CASE(
    "test_fw14_1_backoff_failures_4 [fw-14.1][row-4]",
    "[ws][fw-14.1][backoff]")
{
    reset_state();
    assert_backoff_row(1, 2000u);
    assert_backoff_row(2, 4000u);
    assert_backoff_row(3, 8000u);
    assert_backoff_row(4, 16000u);
}

TEST_CASE(
    "test_fw14_1_backoff_failures_5 [fw-14.1][row-5][cap-reached]",
    "[ws][fw-14.1][backoff]")
{
    reset_state();
    assert_backoff_row(1, 2000u);
    assert_backoff_row(2, 4000u);
    assert_backoff_row(3, 8000u);
    assert_backoff_row(4, 16000u);
    assert_backoff_row(5, 30000u);
}

TEST_CASE(
    "test_fw14_1_backoff_failures_6 [fw-14.1][row-6][cap-holds]",
    "[ws][fw-14.1][backoff]")
{
    reset_state();
    assert_backoff_row(1, 2000u);
    assert_backoff_row(2, 4000u);
    assert_backoff_row(3, 8000u);
    assert_backoff_row(4, 16000u);
    assert_backoff_row(5, 30000u);
    assert_backoff_row(6, 30000u);
}

/* ---------- FW-14.2 — failure-counter lifecycle ---------- */

/* S1: counter resets on CONNECTED. GIVEN consecutive_failures = 4,
 * WHEN the connected semantics fire, THEN counter = 0 and the next
 * disconnect schedules a 2000 ms reconnect again. */
TEST_CASE(
    "test_fw14_2_counter_resets_on_connected [fw-14.2][counter][scenario-S1]",
    "[ws][fw-14.2][counter]")
{
    reset_state();
    (void)ws_backoff_on_failure();
    (void)ws_backoff_on_failure();
    (void)ws_backoff_on_failure();
    (void)ws_backoff_on_failure();
    TEST_ASSERT_EQUAL_UINT32(4, ws_backoff_failure_count());

    ws_backoff_on_connected();

    TEST_ASSERT_EQUAL_UINT32(0, ws_backoff_failure_count());
    TEST_ASSERT_EQUAL_UINT32(0, ws_backoff_current_delay_ms());
    /* Next disconnect schedules row-1 again. */
    assert_backoff_row(1, 2000u);
}

/* S2: counter persists across back-to-back failures with no
 * intervening CONNECTED. The second schedule must be row-2. */
TEST_CASE(
    "test_fw14_2_counter_persists_back_to_back [fw-14.2][counter][scenario-S2]",
    "[ws][fw-14.2][counter]")
{
    reset_state();
    (void)ws_backoff_on_failure();
    (void)ws_backoff_on_failure();

    TEST_ASSERT_EQUAL_UINT32(2, ws_backoff_failure_count());
    TEST_ASSERT_EQUAL_UINT32(4000u, ws_backoff_current_delay_ms());
    TEST_ASSERT_EQUAL_INT(4000,
        mock_esp_websocket_client_get_last_reconnect_timeout_ms());
}

/* S3: CONNECTED clears the clean-CLOSE latch; a later failure
 * schedules normally again. */
TEST_CASE(
    "test_fw14_2_connect_clears_latch [fw-14.2][latch][scenario-S3]",
    "[ws][fw-14.2][latch]")
{
    reset_state();
    ws_backoff_latch_set(true);
    TEST_ASSERT_TRUE(ws_backoff_latch_get());

    ws_backoff_on_connected();

    TEST_ASSERT_FALSE(ws_backoff_latch_get());
    assert_backoff_row(1, 2000u);
}
