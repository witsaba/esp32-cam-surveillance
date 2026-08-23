/* test_ws_uri_no_mac.c — FW-13.1 (URI = /cams, TCP transport, no MAC).
 *
 * Three scenarios from REQ-WS-001:
 *   S1 — URI path is exactly "/cams"
 *   S2 — transport == WEBSOCKET_TRANSPORT_OVER_TCP
 *   S3 — no MAC substring anywhere in the URI
 *
 * Plus the lazy-start invariant: ws_init() does NOT call
 * esp_websocket_client_start (the actual start happens in the IP-up
 * handler — this prevents DNS-fails-before-STA-up failure mode per
 * charter L1207 + design #3756 §4). The lazy-start assertion is
 * folded into the third test (no extra test needed; the mock's
 * start_call_count == 0 after ws_init() is a direct observation).
 *
 * Each test exercises a real ws_init(cfg) path; the mock
 * mock_esp_websocket_client captures the config that ws.c built.
 * Tests use a minimal config_t (cfg->wifi.ssid set; name/description
 * empty so identity_load reads empty NVS values without logging
 * noise beyond ESP_LOGW which the host mock silences).
 *
 * Test convention matches the codebase (FW-08 / FW-10 / FW-11):
 * Unity TEST_CASE with [group] tags — discovered by Unity's
 * TEST_CASE_REGISTRY + the host_test_main runner.
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
#include "mock_nvs_flash_link.h"
#include "mock_esp_system_link.h"
#endif

/* Module-static config shared across tests. The boot orchestrator
 * passes its own cfg pointer, but the test owns the struct so we
 * don't need a real NVS load. Reset between tests via Unity's
 * setUp/tearDown hooks (we use a per-test reset; no setUp needed
 * because Unity isolates cases). */
static config_t s_test_cfg;

/* Per-test setup: reset mock + prime config. */
static void reset_state(void)
{
    memset(&s_test_cfg, 0, sizeof(s_test_cfg));
    /* SSID non-empty so wifi_init would pass validation if any
     * transitive call surfaces. */
    strncpy(s_test_cfg.wifi.ssid, "TestSSID",
            sizeof(s_test_cfg.wifi.ssid) - 1);
    s_test_cfg.wifi.ssid[sizeof(s_test_cfg.wifi.ssid) - 1] = '\0';
#ifdef UNITY_HOST_BUILD
    mock_esp_websocket_client_reset_for_test();
#endif
}

/* ---------- REQ-WS-001 S1: URI path segment == "/cams" ---------- */
TEST_CASE(
    "test_uri_path_is_cams [fw-13.1][uri-no-mac][scenario-S1]",
    "[ws][fw-13.1][uri-no-mac]")
{
    reset_state();

    /* Drive ws_init. On host, ws.c's ws_init_impl body should
     * call esp_websocket_client_init with the URI built from
     * CONFIG_FIRMWARE_WS_URI_DEFAULT + CONFIG_FIRMWARE_WS_PATH. */
    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* Pull the captured URI from the mock. */
    const char *uri = mock_esp_websocket_client_get_last_uri();
    TEST_ASSERT_NOT_NULL_MESSAGE(uri, "URI must be captured by mock");

    /* Parse the path segment (between the third '/' and the
     * end-of-string or '?' fragment). The ws_text_frame_parse_
     * uri_path helper is the canonical parser; we use it here
     * to extract the path. Sized to comfortably hold "/cams"
     * + NUL (literal cap — CONFIG_FIRMWARE_WS_PATH_MAX_LEN is
     * not yet a Kconfig symbol; this constant mirrors the
     * charter L1180 + ws_text_frame.c buffer convention). */
    char path[64] = {0};
    r = ws_text_frame_parse_uri_path(uri, path, sizeof(path));
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* Spec invariant: path segment is EXACTLY "/cams". No MAC,
     * no query, no fragment. */
    TEST_ASSERT_EQUAL_STRING("/cams", path);

    /* Lazy-start invariant: ws_init() must NOT call
     * esp_websocket_client_start (the actual start happens in
     * the IP-up handler). The mock's start_call_count == 0
     * is a direct observation. */
    TEST_ASSERT_EQUAL_UINT(0, mock_esp_websocket_client_start_call_count());
}

/* ---------- REQ-WS-001 S2: TCP transport ---------- */
TEST_CASE(
    "test_transport_is_tcp [fw-13.1][uri-no-mac][scenario-S2]",
    "[ws][fw-13.1][uri-no-mac]")
{
    reset_state();

    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    esp_websocket_transport_t t = mock_esp_websocket_client_get_transport();
    TEST_ASSERT_EQUAL(WEBSOCKET_TRANSPORT_OVER_TCP, t);

    /* Spec invariant: disable_auto_reconnect MUST be true
     * (FW-14 owns reconnect — IDF built-in races the FW-14
     * counter producer). */
    TEST_ASSERT_TRUE(mock_esp_websocket_client_get_disable_auto_reconnect());
}

/* ---------- REQ-WS-001 S3: no MAC substring in URI ----------
 *
 * Verifies the charter invariant: the URI MUST NOT contain the
 * MAC anywhere. This is the green-path arm of the FW-13.4
 * URL-no-MAC guard (the bite-proof runs under Pass 11 with the
 * mock forced to inject the MAC into the URI — T-13-F lands
 * that tripwire). */
TEST_CASE(
    "test_no_mac_in_uri [fw-13.1][uri-no-mac][scenario-S3]",
    "[ws][fw-13.1][uri-no-mac]")
{
    reset_state();

    /* The URL must contain zero MAC substrings. We assert no
     * 12-char lowercase-hex substring matches anywhere — the
     * URI builder never splices identity into the path/query
     * (the charter invariant + FW-13.4 URL-no-MAC guard). */

    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    const char *uri = mock_esp_websocket_client_get_last_uri();
    TEST_ASSERT_NOT_NULL(uri);

    /* The URI is composed from CONFIG_FIRMWARE_WS_URI_DEFAULT +
     * CONFIG_FIRMWARE_WS_PATH. Both are fixed literal strings on
     * host (mirrored via -D cflags). We assert the literal
     * contains NO 12-char hex substring by scanning for any
     * "x0123456789abcdef" pattern (defence-in-depth: a future
     * bug that splices MAC would surface here). */
    size_t n = strlen(uri);
    int hits = 0;
    for (size_t i = 0; i + 12 <= n; ++i) {
        bool all_hex = true;
        for (int k = 0; k < 12; ++k) {
            char c = uri[i + k];
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) {
                all_hex = false;
                break;
            }
        }
        if (all_hex) hits++;
    }
    /* The host's default URI is "ws://example.local:9000/cams"
     * — no 12-char hex substring possible. Assert zero hits. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits,
        "URI must contain no 12-char hex substring (MAC leak guard)");
}
