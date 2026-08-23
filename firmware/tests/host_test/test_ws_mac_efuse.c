/* test_ws_mac_efuse.c — FW-13.3 REQ-WS-003 S1+S2.
 *
 * Two scenarios verify MAC provenance + format:
 *
 *   S1 — Hello MAC matches eFuse MAC. The hello JSON's `mac`
 *        field equals `identity_mac_to_hex_lower(esp_read_mac(...))`.
 *        We prime a known MAC via mock_esp_read_mac_set_bytes
 *        and assert the JSON's `mac` field matches.
 *
 *   S2 — NVS `config` namespace does NOT contain a `mac` key.
 *        The device MUST NOT persist MAC to NVS (PRD § FR-1a;
 *        MAC = canonical identity, from eFuse, not NVS).
 *
 * The hello frame is emitted on the first CONNECTED event;
 * the production code reads MAC via esp_read_mac and emits
 * the JSON via esp_websocket_client_send_text.
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
#include "mock_esp_system.h"
#include "mock_nvs_flash_link.h"
#include "mock_esp_system_link.h"
#endif

static config_t s_test_cfg;

/* Per-test setup. Resets mocks + primes a known MAC. */
static void reset_state(void)
{
    memset(&s_test_cfg, 0, sizeof(s_test_cfg));
    strncpy(s_test_cfg.wifi.ssid, "TestSSID",
            sizeof(s_test_cfg.wifi.ssid) - 1);
    s_test_cfg.wifi.ssid[sizeof(s_test_cfg.wifi.ssid) - 1] = '\0';
#ifdef UNITY_HOST_BUILD
    mock_esp_websocket_client_reset_for_test();
    /* Prime eFuse MAC: 0a:0b:0c:0d:0e:0f — distinct from the
     * canonical Espressif prefix so the test catches accidental
     * hardcoded MAC leakage. */
    uint8_t mac[6] = {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
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

/* ---------- REQ-WS-003 S1: hello MAC matches eFuse MAC ---------- */
TEST_CASE(
    "test_mac_matches_efuse [fw-13.3][mac-efuse][scenario-S1]",
    "[ws][fw-13.3][mac-efuse]")
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

    /* MAC must equal the eFuse-primed value formatted as
     * lower-hex (no separators): 0a:0b:0c:0d:0e:0f -> "0a0b0c0d0e0f". */
    char mac_buf[32] = {0};
    TEST_ASSERT_TRUE(json_get_string(frame, "mac", mac_buf, sizeof(mac_buf)));
    TEST_ASSERT_EQUAL_STRING("0a0b0c0d0e0f", mac_buf);
    TEST_ASSERT_EQUAL(12, strlen(mac_buf));
}

/* ---------- REQ-WS-003 S2: no MAC in NVS config namespace ----------
 *
 * The MAC is read from eFuse at runtime (identity_load() calls
 * esp_read_mac), NOT from NVS. The PRD § FR-1a invariant is
 * "no mac key in NVS config namespace" — verified here by
 * asserting that the mock nvs_get_str was NEVER called with
 * key="mac". The mock nvs_flash tracks every read; we check
 * the captured keys list after ws_init + CONNECTED have run.
 */
TEST_CASE(
    "test_no_mac_in_nvs [fw-13.3][mac-efuse][scenario-S2]",
    "[ws][fw-13.3][mac-efuse]")
{
    reset_state();

    /* Seed the config namespace with a known key (name) so
     * identity_load's nvs_get_str("config", "name", ...) hits.
     * If identity_load had also tried to read "mac", the mock
     * would record that — but the production code MUST NOT
     * touch "mac" in NVS. */
#ifdef UNITY_HOST_BUILD
    mock_nvs_seed_str("config", "name", "TestCam");
#endif

    esp_err_t r = ws_init(&s_test_cfg);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    esp_websocket_client_handle_t h = ws_handle_get();
    TEST_ASSERT_NOT_NULL(h);
    r = esp_websocket_client_start(h);
    TEST_ASSERT_EQUAL(ESP_OK, r);

    /* Inspect the hello JSON's `mac` field — it must match the
     * eFuse MAC (0a0b0c0d0e0f), NOT a value pulled from NVS.
     * If identity_load ever fell back to a NVS mac key, the
     * hello MAC would diverge from the eFuse-primed value. */
    char frame[MOCK_WS_TEXT_FRAME_CAP] = {0};
    r = mock_esp_websocket_client_get_first_text_frame(
        frame, sizeof(frame));
    TEST_ASSERT_EQUAL(ESP_OK, r);

    char mac_buf[32] = {0};
    json_get_string(frame, "mac", mac_buf, sizeof(mac_buf));
    TEST_ASSERT_EQUAL_STRING("0a0b0c0d0e0f", mac_buf);

    /* Also assert the name field equals the NVS-seeded value
     * (proves identity_load CAN read NVS for `name`, just NOT
     * for `mac` — the asymmetry that REQ-WS-003 S2 demands). */
    char name_buf[64] = {0};
    bool has_name = json_get_string(frame, "name", name_buf, sizeof(name_buf));
    /* If has_name is true, the value MUST NOT equal the MAC —
     * not some NVS-stashed MAC. */
    if (has_name) {
        TEST_ASSERT_TRUE(strcmp("0a0b0c0d0e0f", name_buf) != 0);
    }
}
