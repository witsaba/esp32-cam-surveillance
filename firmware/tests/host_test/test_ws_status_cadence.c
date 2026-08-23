/* test_ws_status_cadence.c — FW-13.5 REQ-WS-005 (status cadence).
 *
 * Two scenarios verify the 30 s status frame cadence:
 *
 *   S1 — 3 frames in 90 s. Drive ws_init → esp_websocket_client
 *       _start (CONNECTED → hello emit → status timer start).
 *       Advance the periodic timer by 90 s via the host mock's
 *       mock_esp_timer_advance_periodic helper. Assert the WS
 *       mock captured 1 hello + 3 status frames (total = 4
 *       text frames in the ring buffer).
 *
 *   S2 — 0 frames when disconnected. Drive CONNECTED → 1 hello.
 *       Fire DISCONNECTED (mock fires the registered handler
 *       synchronously). Advance 60 s. Assert no additional
 *       frames are emitted (the timer is stopped).
 */
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <string.h>

#include "ws.h"
#include "config.h"
#include "identity.h"
#include "esp_err.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_websocket_client.h"
#include "mock_esp_timer.h"
#include "mock_esp_event.h"
#include "mock_nvs_flash_link.h"
#include "mock_esp_system_link.h"
#include "mock_init_returns.h"

/* Host-only reset for the ws_status_timer module-static
 * handle. Without this, tests that follow a previous test
 * (which had its handle slot wiped by mock_esp_timer_reset)
 * see a stale g_status_timer pointer that no longer maps to a
 * real mock slot — advance_periodic returns ESP_ERR_INVALID_ARG
 * and no status callbacks fire. */
extern void ws_status_timer_reset_handle_for_test(void);
#endif

static config_t s_test_cfg;

static void reset_state(void)
{
    memset(&s_test_cfg, 0, sizeof(s_test_cfg));
    strncpy(s_test_cfg.wifi.ssid, "TestSSID",
            sizeof(s_test_cfg.wifi.ssid) - 1);
    s_test_cfg.wifi.ssid[sizeof(s_test_cfg.wifi.ssid) - 1] = '\0';
#ifdef UNITY_HOST_BUILD
    mock_init_returns_reset();
    mock_esp_websocket_client_reset_for_test();
    /* The host runner's main() doesn't reset the timer mock
     * between passes; do it explicitly so our handle_count is
     * predictable. */
    mock_esp_timer_reset();
    /* Clear the ws_status_timer module-static handle so the
     * next ws_init re-creates the timer slot in the freshly-
     * cleared mock table. Without this the test sees a stale
     * g_status_timer pointer that no longer maps to a real
     * mock slot — advance_periodic fails with NO_SLOT and no
     * status callbacks fire. */
    ws_status_timer_reset_handle_for_test();
    /* Reset event-mock slot table — wifi + ws subscriptions
     * accumulate across tests within a single binary. The
     * 8-slot table fills fast; reset between tests to keep
     * ws_event_handler_install's wifi_event_subscribe call
     * from returning ESP_ERR_NO_MEM. */
    mock_esp_event_reset();
    ws_event_handler_reset_for_test();
    /* Prime eFuse MAC for the hello emit. */
    uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0x9d, 0x50, 0x08};
    mock_esp_read_mac_set_bytes(mac);
#endif
}

/* ---------- REQ-WS-005 S1: 3 frames in 90 s ---------- */
TEST_CASE(
    "test_status_cadence_3_frames_in_90s [fw-13.5][status-cadence][scenario-S1]",
    "[ws][fw-13.5][status-cadence]")
{
    reset_state();

    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* Start the WS client → CONNECTED → on_ws_connected fires
     * (emits hello + starts the periodic status timer). */
    esp_websocket_client_handle_t h = ws_handle_get();
    TEST_ASSERT_NOT_NULL(h);
    r = esp_websocket_client_start(h);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* 1 hello should have been emitted. */
    TEST_ASSERT_EQUAL_UINT(1,
        mock_esp_websocket_client_send_text_call_count());

    /* Grab the periodic timer handle and advance 90 s — the
     * mock's helper computes n_ticks = advance_ms / period_ms
     * and fires the callback n times. For period=30000 ms and
     * advance=90000 ms, n_ticks=3 (the charter invariant). */
#ifdef UNITY_HOST_BUILD
    void *timer_handle = ws_status_timer_handle_get();
    if (timer_handle) {
        mock_esp_timer_advance_periodic(timer_handle, 90000);
    }
#endif

    /* After hello + 3 status frames: send_text called 4 times. */
    size_t expected_calls = 4;
    size_t actual_calls =
        mock_esp_websocket_client_send_text_call_count();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(expected_calls, actual_calls,
        "expected 1 hello + 3 status frames after 90 s of timer advance");
}

/* ---------- REQ-WS-005 S2: 0 frames when disconnected ---------- */
TEST_CASE(
    "test_status_cadence_0_frames_when_disconnected [fw-13.5][status-cadence][scenario-S2]",
    "[ws][fw-13.5][status-cadence]")
{
    reset_state();

    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    esp_websocket_client_handle_t h = ws_handle_get();
    TEST_ASSERT_NOT_NULL(h);
    r = esp_websocket_client_start(h);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* 1 hello emitted. */
    TEST_ASSERT_EQUAL_UINT(1,
        mock_esp_websocket_client_send_text_call_count());

    /* Fire DISCONNECTED — the registered handler should stop
     * the status timer. */
    mock_esp_websocket_client_fire_disconnected();

    /* Advance 60 s of timer — the timer should be stopped, so
     * no additional frames should emit. */
#ifdef UNITY_HOST_BUILD
    void *timer_handle = ws_status_timer_handle_get();
    if (timer_handle) {
        mock_esp_timer_advance_periodic(timer_handle, 60000);
    }
#endif

    /* Only the hello from CONNECTED; no status frames. */
    TEST_ASSERT_EQUAL_UINT(1,
        mock_esp_websocket_client_send_text_call_count());
}
