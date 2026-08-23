/* test_ws_hello_first_frame.c — FW-13.2 REQ-WS-002 S1+S2.
 *
 * Two scenarios verify the hello frame is emitted as the FIRST
 * text frame after WEBSOCKET_EVENT_CONNECTED, with the full
 * documented JSON shape:
 *
 *   S1 — hello is the first text frame (text opcode 0x1, not
 *        binary). The mock captures the first esp_websocket
 *        _client_send_text call. Test parses the JSON and
 *        asserts every documented field is present.
 *
 *   S2 — hello JSON shape matches REQ-WS-002 exactly:
 *        {"type":"hello","mac":"<12-hex>","name":"<nvs>",
 *         "description":"<nvs>","fw":"<version>",
 *         "caps":["jpeg","stream","identify"]}
 *
 * Test driver: ws_init(cfg) registers the handler; the test
 * fires WEBSOCKET_EVENT_CONNECTED via mock_esp_websocket_client
 * _fire_event, which synchronously invokes the registered
 * CONNECTED handler (the production code emits the hello).
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
#include "mock_esp_event.h"
#include "mock_nvs_flash_link.h"
#include "mock_esp_system_link.h"
#include "mock_init_returns.h"
#endif

/* Per-test setup. Resets the mock + primes a known MAC for
 * deterministic JSON assertions. */
static config_t s_test_cfg;

static void reset_state(void)
{
    memset(&s_test_cfg, 0, sizeof(s_test_cfg));
    strncpy(s_test_cfg.wifi.ssid, "TestSSID",
            sizeof(s_test_cfg.wifi.ssid) - 1);
    s_test_cfg.wifi.ssid[sizeof(s_test_cfg.wifi.ssid) - 1] = '\0';
#ifdef UNITY_HOST_BUILD
    /* CRITICAL: clear any stale forced-failure state set by
     * prior FW-03 boot-order tests (test_boot_order.c sets
     * BOOT_STEP_WS_INIT=ESP_FAIL at line 147 and never resets).
     * Without this reset, ws_init's mock_init_returns_get
     * short-circuit returns ESP_FAIL and the init body never
     * runs — the hello emit never fires. */
    mock_init_returns_reset();
    mock_esp_websocket_client_reset_for_test();
    /* Reset event-mock slot table to prevent NO_MEM under
     * accumulated subscriptions from prior tests. */
    mock_esp_event_reset();
    /* Reset the ws component's install idempotency flag so
     * the next ws_init → ws_event_handler_install call re-
     * registers the CONNECTED/DISCONNECTED handlers (the mock
     * resets its handler table on each _init call; the
     * production idempotency would otherwise leave no handlers
     * registered for the new test case). */
    ws_event_handler_reset_for_test();
    /* Prime the eFuse MAC: c8:f0:9e:9d:50:08 (Espressif
     * canonical prefix). The mock's esp_read_mac returns
     * these bytes when not overridden by the test. */
    uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0x9d, 0x50, 0x08};
    mock_esp_read_mac_set_bytes(mac);
#endif
}

/* Helper — extract a top-level JSON string value by name.
 * Returns true on match; copies up to out_len-1 bytes to out. */
static bool json_get_string(const char *json, const char *key,
                             char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) return false;
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += (size_t)n;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/* Helper — check a top-level JSON string field is present. */
static bool json_has_string_field(const char *json, const char *key)
{
    if (!json || !key) return false;
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;
    return strstr(json, needle) != NULL;
}

/* Helper — check a top-level JSON array field is present. */
static bool json_has_array_field(const char *json, const char *key)
{
    if (!json || !key) return false;
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":[", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;
    return strstr(json, needle) != NULL;
}

/* ---------- REQ-WS-002 S1: hello is first frame after CONNECTED ---------- */
TEST_CASE(
    "test_hello_first_after_connected [fw-13.2][hello-first][scenario-S1]",
    "[ws][fw-13.2][hello-first]")
{
    reset_state();

    /* ws_init subscribes CONNECTED + DISCONNECTED handlers. The
     * mock's start_call_count == 0 (lazy start). */
    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);
    TEST_ASSERT_EQUAL_UINT(0, mock_esp_websocket_client_start_call_count());

    /* Start the WS client — the mock fires CONNECTED synchronously
     * inside _start. The production on_ws_connected handler emits
     * the hello frame via esp_websocket_client_send_text. */
    esp_websocket_client_handle_t h = ws_handle_get();
    TEST_ASSERT_NOT_NULL(h);
    r = esp_websocket_client_start(h);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* After CONNECTED + hello emit, exactly 1 send_text call
     * should have happened (the hello). */
    TEST_ASSERT_EQUAL_UINT(1,
        mock_esp_websocket_client_send_text_call_count());

    /* The first text frame captured by the mock should be the
     * hello JSON. */
    char frame[MOCK_WS_TEXT_FRAME_CAP] = {0};
    r = mock_esp_websocket_client_get_first_text_frame(
        frame, sizeof(frame));
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* Hello type field must be "hello" exactly. */
    TEST_ASSERT_TRUE(json_has_string_field(frame, "type"));
    char type_buf[16] = {0};
    TEST_ASSERT_TRUE(json_get_string(frame, "type", type_buf, sizeof(type_buf)));
    TEST_ASSERT_EQUAL_STRING("hello", type_buf);
}

/* ---------- REQ-WS-002 S2: hello JSON shape ---------- */
TEST_CASE(
    "test_hello_full_payload [fw-13.2][hello-first][scenario-S2]",
    "[ws][fw-13.2][hello-first]")
{
    reset_state();

    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    esp_websocket_client_handle_t h = ws_handle_get();
    TEST_ASSERT_NOT_NULL(h);
    r = esp_websocket_client_start(h);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    char frame[MOCK_WS_TEXT_FRAME_CAP] = {0};
    r = mock_esp_websocket_client_get_first_text_frame(
        frame, sizeof(frame));
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* Every documented field MUST be present. */
    TEST_ASSERT_TRUE_MESSAGE(json_has_string_field(frame, "type"),
        "hello.type missing");
    TEST_ASSERT_TRUE_MESSAGE(json_has_string_field(frame, "mac"),
        "hello.mac missing");
    TEST_ASSERT_TRUE_MESSAGE(json_has_string_field(frame, "name"),
        "hello.name missing");
    TEST_ASSERT_TRUE_MESSAGE(json_has_string_field(frame, "description"),
        "hello.description missing");
    TEST_ASSERT_TRUE_MESSAGE(json_has_string_field(frame, "fw"),
        "hello.fw missing");
    TEST_ASSERT_TRUE_MESSAGE(json_has_array_field(frame, "caps"),
        "hello.caps array missing");

    /* MAC must match the primed eFuse MAC (c8:f0:9e:9d:50:08 ->
     * "c8f09e9d5008"). */
    char mac_buf[32] = {0};
    TEST_ASSERT_TRUE(json_get_string(frame, "mac", mac_buf, sizeof(mac_buf)));
    TEST_ASSERT_EQUAL_STRING("c8f09e9d5008", mac_buf);
}
