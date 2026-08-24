/* test_ws_status_payload.c — FW-13.6 REQ-WS-006 (status payload;
 * FW-16 server-mode fixture).
 *
 * Three scenarios verify the status frame carries the full
 * documented 8-field payload with correct types:
 *
 *   S1 — Full payload present. Parse the status JSON and
 *       assert every documented key is present with the
 *       expected type.
 *   S2 — reconnects == 0. The fw-13 charter L1201 reserves the
 *       producer for FW-14; the stub returns 0.
 *   S3 — rssi_dbm reflects the mock. Prime
 *       mock_esp_wifi_set_rssi_dbm(-47); assert the status field
 *       equals -47.
 *
 * Driver (server mode): ws_init arms module state + creates the
 * periodic handle; the recorder sink stands in for the connected
 * viewer; arming the timer directly mirrors viewer-accept and
 * one 30 s advance drives exactly one status emission.
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
#include "mock_esp_timer.h"
#include "mock_esp_wifi.h"
#include "mock_esp_system.h"
#include "mock_nvs_flash_link.h"
#include "mock_init_returns.h"
#include "ws_sink_recorder.h"
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
    mock_esp_timer_reset();
    ws_status_timer_reset_handle_for_test();
    ws_sink_recorder_reset();
    /* Prime eFuse MAC for the identity fields. */
    uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0x9d, 0x50, 0x08};
    mock_esp_read_mac_set_bytes(mac);
#endif
}

/* Helper — extract a top-level JSON string value by name. */
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

/* Helper — extract a top-level JSON number value by name.
 * Returns true on match; copies digits to out (NUL-terminated). */
static bool json_get_number(const char *json, const char *key,
                             char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) return false;
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += (size_t)n;
    while (*p == ' ') p++;
    const char *start = p;
    if (*p == '-') p++;
    while (*p >= '0' && *p <= '9') p++;
    size_t len = (size_t)(p - start);
    if (len == 0 || len >= out_len) return false;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

/* Helper — arm the timer (viewer-accept equivalent) + advance one
 * 30 s period to drive exactly one status emission. The recorder
 * sink receives the frame at index 0 (oldest-first ring, no hello
 * in this fixture). */
static void drive_status_cycle(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, ws_status_timer_start());
#ifdef UNITY_HOST_BUILD
    void *timer_handle = ws_status_timer_handle_get();
    if (timer_handle) {
        mock_esp_timer_advance_periodic(timer_handle, 30000);
    }
#endif
}

static esp_err_t read_status_frame(char *frame, size_t cap)
{
    return ws_sink_recorder_get_text_at(0, frame, cap);
}

/* ---------- REQ-WS-006 S1: full payload ---------- */
TEST_CASE(
    "test_status_payload_full_fields [fw-13.6][status-payload][scenario-S1]",
    "[ws][fw-13.6][status-payload]")
{
    reset_state();

    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);
    TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_install());

    drive_status_cycle();

    char frame[256] = {0};
    r = read_status_frame(frame, sizeof(frame));
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* type field must be "status" exactly. */
    char type_buf[16] = {0};
    TEST_ASSERT_TRUE(json_get_string(frame, "type", type_buf,
                                       sizeof(type_buf)));
    TEST_ASSERT_EQUAL_STRING("status", type_buf);

    /* Every documented key MUST be present. */
    TEST_ASSERT_TRUE_MESSAGE(json_get_string(frame, "mac", (char[32]){0},
                                              32),
        "status.mac missing");
    TEST_ASSERT_TRUE_MESSAGE(json_get_string(frame, "name", (char[64]){0},
                                              64),
        "status.name missing");
    char num_buf[32];
    TEST_ASSERT_TRUE_MESSAGE(json_get_number(frame, "uptime_s", num_buf,
                                              sizeof(num_buf)),
        "status.uptime_s missing");
    TEST_ASSERT_TRUE_MESSAGE(json_get_number(frame, "rssi_dbm", num_buf,
                                              sizeof(num_buf)),
        "status.rssi_dbm missing");
    TEST_ASSERT_TRUE_MESSAGE(json_get_number(frame, "free_heap", num_buf,
                                              sizeof(num_buf)),
        "status.free_heap missing");
    TEST_ASSERT_TRUE_MESSAGE(json_get_number(frame, "fb_drops", num_buf,
                                              sizeof(num_buf)),
        "status.fb_drops missing");
    TEST_ASSERT_TRUE_MESSAGE(json_get_number(frame, "reconnects", num_buf,
                                              sizeof(num_buf)),
        "status.reconnects missing");
}

/* ---------- REQ-WS-006 S2: reconnects == 0 in FW-13 ---------- */
TEST_CASE(
    "test_status_reconnects_zero_in_fw13 [fw-13.6][status-payload][scenario-S2]",
    "[ws][fw-13.6][status-payload]")
{
    reset_state();

    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);
    TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_install());

    drive_status_cycle();

    char frame[256] = {0};
    r = read_status_frame(frame, sizeof(frame));
    TEST_ASSERT_EQUAL(ESP_OK, r);

    char num_buf[32] = {0};
    TEST_ASSERT_TRUE(json_get_number(frame, "reconnects", num_buf,
                                       sizeof(num_buf)));
    int reconnects = atoi(num_buf);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, reconnects,
        "status.reconnects must be 0 in FW-13 (FW-14 owns the producer)");
}

/* ---------- REQ-WS-006 S3: rssi_dbm reflects mock ---------- */
TEST_CASE(
    "test_status_rssi_reflects_mock [fw-13.6][status-payload][scenario-S3]",
    "[ws][fw-13.6][status-payload]")
{
    reset_state();

#ifdef UNITY_HOST_BUILD
    /* Prime the RSSI mock to a deterministic value. */
    mock_esp_wifi_set_rssi_dbm(-47);
#endif

    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);
    TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_install());

    drive_status_cycle();

    char frame[256] = {0};
    r = read_status_frame(frame, sizeof(frame));
    TEST_ASSERT_EQUAL(ESP_OK, r);

    char num_buf[32] = {0};
    TEST_ASSERT_TRUE(json_get_number(frame, "rssi_dbm", num_buf,
                                       sizeof(num_buf)));
    int rssi = atoi(num_buf);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-47, rssi,
        "status.rssi_dbm must reflect the mock's primed value");
}
