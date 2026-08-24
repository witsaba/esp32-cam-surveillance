/* test_ws_server.c — FW-16 device-as-server WebSocket endpoint.
 *
 * Consolidated server-mode surface (supersedes the client-era
 * suites test_ws_hello_first_frame.c, test_ws_uri_no_mac.c,
 * test_ws_url_guard.c, test_ws_status_cadence.c and the hello
 * arms of test_ws_mac_efuse.c):
 *
 *   S1 — /cams registers on IP-up with HTTP_GET +
 *        is_websocket=true; endpoint path contains no MAC
 *        substring (identity-leak guard carried over from
 *        FW-13.1/FW-13.4).
 *   S2 — handshake accept: exactly ONE text frame (hello) is the
 *        FIRST frame of the session, with the full REQ-WS-002
 *        JSON shape; mac comes from eFuse while name comes from
 *        NVS (REQ-WS-003 asymmetry preserved).
 *   S3 — single-viewer policy: a second handshake on another fd
 *        gets ESP_FAIL + a viewer_limit error frame; the active
 *        session is untouched.
 *   S4 — socket close of THE viewer frees the slot: sink goes
 *        disconnected and the next handshake is accepted again.
 *   S5 — status cadence: 1 hello + 3 status frames per 90 s
 *        (REQ-WS-005 S1 semantics, timer-advance driven).
 *   S6 — after viewer close, advancing the timer emits nothing.
 *
 * Driver: softap_sta_listener_install() + mock IP-up event start
 * the httpd mock registry (mirrors test_softap_sta_listener.c);
 * handshakes run through mock_httpd_invoke_registered_handler.
 */
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <string.h>

#include "esp_err.h"

#include "ws.h"
#include "ws_server.h"
#include "config.h"
#include "identity.h"
#include "softap.h"

#include "mock_esp_event.h"
#include "mock_esp_event_link.h"   /* IP_EVENT base */
#include "mock_http_server.h"
#include "mock_esp_timer.h"
#include "mock_esp_system.h"
#include "mock_nvs_flash_link.h"
#include "mock_init_returns.h"
#include "esp_event.h"

/* Server-mode emission goes through the httpd async-send path
 * (httpd_ws_send_frame_async with hd+fd captured at accept), so
 * this suite reads emitted frames from the mock's WS ring — NOT
 * from the sink recorder (that stands in for the viewer sink only
 * in suites that never perform a handshake). */

extern void softap_sta_listener_reset_for_test(void);
extern void ws_status_timer_reset_handle_for_test(void);

static void reset_state(void)
{
    mock_httpd_reset();
    mock_esp_event_reset();
    mock_init_returns_reset();
    mock_esp_timer_reset();
    softap_sta_listener_reset_for_test();
    ws_server_reset_for_test();
    ws_status_timer_reset_handle_for_test();

    uint8_t mac[6] = {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    mock_esp_read_mac_set_bytes(mac);
}

/* Bring the STA listener AND the ws attach hook up through the
 * real IP-up path so the /cams endpoint lands in the httpd mock
 * registry (softap starts the server; ws attaches the endpoint —
 * production order: listener installs before ws_init). */
static void server_up(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, softap_sta_listener_install());
    TEST_ASSERT_EQUAL(ESP_OK, ws_server_install());
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_esp_event_fire_handler(IP_EVENT,
                                    IP_EVENT_STA_GOT_IP,
                                    NULL));
}

/* Drive one WS opening handshake against the /cams registration.
 * `fd` primes the socket id the handler captures. Returns the
 * handler's esp_err_t. */
static esp_err_t handshake(int fd)
{
    mock_httpd_req_t *req = mock_httpd_req_new();
    req->method = HTTP_GET;
    req->sockfd = fd;
    esp_err_t rc = mock_httpd_invoke_registered_handler(
        CONFIG_FIRMWARE_WS_PATH, HTTP_GET, req);
    mock_httpd_req_free(req);
    return rc;
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

/* ---------- S1: endpoint registration + identity-leak guard ---------- */
TEST_CASE(
    "test_fw16_cams_endpoint_registers_as_websocket [fw-16][server][scenario-S1]",
    "[ws-server][fw-16][registration]")
{
    reset_state();
    server_up();

    /* Three registrations: /whoami + /snapshot + /cams. */
    TEST_ASSERT_EQUAL_INT(3,
        mock_httpd_register_uri_handler_call_count());

    /* The newest registration is the WS endpoint. */
    const char *uri = NULL;
    int method = -1;
    mock_httpd_last_registered_uri(&uri, &method);
    TEST_ASSERT_NOT_NULL(uri);
    TEST_ASSERT_EQUAL_STRING(CONFIG_FIRMWARE_WS_PATH, uri);
    TEST_ASSERT_EQUAL_INT(HTTP_GET, method);

    bool is_ws = false;
    mock_httpd_last_registered_is_websocket(&is_ws);
    TEST_ASSERT_TRUE(is_ws);

    /* Identity-leak guard (carried over from FW-13.1/FW-13.4):
     * the registered endpoint path contains NO 12-char lowercase-
     * hex substring anywhere. */
    size_t n = strlen(uri);
    int hits = 0;
    for (size_t i = 0; i + 12 <= n; ++i) {
        bool all_hex = true;
        for (int k = 0; k < 12; ++k) {
            char c = uri[i + k];
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f'))) {
                all_hex = false;
                break;
            }
        }
        if (all_hex) hits++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits,
        "endpoint path must contain no 12-char hex substring "
        "(MAC leak guard)");
}

/* ---------- S2: hello-on-accept (first frame + shape + provenance) ---------- */
TEST_CASE(
    "test_fw16_hello_emitted_once_on_viewer_accept [fw-16][server][scenario-S2]",
    "[ws-server][fw-16][hello]")
{
    reset_state();
    /* Seed NVS name so the hello proves the eFuse/NVS asymmetry:
     * mac from eFuse (0a0b0c0d0e0f), name from NVS. */
    mock_nvs_seed_str("config", "name", "TestCam");

    server_up();
    TEST_ASSERT_EQUAL(ESP_OK, handshake(7));

    TEST_ASSERT_TRUE(ws_server_viewer_active());

    /* Exactly ONE frame emitted — and it is TEXT (hello first). */
    TEST_ASSERT_EQUAL_INT(1, mock_httpd_ws_send_call_count());

    char frame[256] = {0};
    int type = -1;
    size_t frame_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, mock_httpd_ws_get_frame_at(
        0, &type, (uint8_t *)frame, sizeof(frame) - 1, &frame_len));
    frame[frame_len] = '\0';
    TEST_ASSERT_EQUAL_INT(HTTPD_WS_TYPE_TEXT, type);

    /* Full REQ-WS-002 shape. */
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

    char type_buf[16] = {0};
    TEST_ASSERT_TRUE(json_get_string(frame, "type",
        type_buf, sizeof(type_buf)));
    TEST_ASSERT_EQUAL_STRING("hello", type_buf);

    /* MAC matches the primed eFuse value, NOT anything from NVS
     * (REQ-WS-003 S1). */
    char mac_buf[32] = {0};
    TEST_ASSERT_TRUE(json_get_string(frame, "mac",
        mac_buf, sizeof(mac_buf)));
    TEST_ASSERT_EQUAL_STRING("0a0b0c0d0e0f", mac_buf);
    TEST_ASSERT_EQUAL_INT(12, (int)strlen(mac_buf));

    /* Name DOES come from NVS (the asymmetry REQ-WS-003 S2
     * demands — identity_load reads name from NVS, never mac). */
    char name_buf[64] = {0};
    TEST_ASSERT_TRUE(json_get_string(frame, "name",
        name_buf, sizeof(name_buf)));
    TEST_ASSERT_EQUAL_STRING("TestCam", name_buf);
}

/* ---------- S3: single-viewer policy rejects a second handshake ---------- */
TEST_CASE(
    "test_fw16_second_handshake_rejected_viewer_limit [fw-16][server][scenario-S3]",
    "[ws-server][fw-16][single-viewer]")
{
    reset_state();
    server_up();

    TEST_ASSERT_EQUAL(ESP_OK, handshake(7));   /* first viewer in */
    TEST_ASSERT_TRUE(ws_server_viewer_active());

    /* Second handshake from ANOTHER socket → rejected hard. */
    esp_err_t rc = handshake(9);
    TEST_ASSERT_EQUAL(ESP_FAIL, rc);

    /* Ring: hello (idx 0, to fd 7) then rejection (idx 1, to the
     * second handshake socket) — both via httpd async-send. */
    TEST_ASSERT_EQUAL_INT(2, mock_httpd_ws_send_call_count());

    int type = -1;
    uint8_t body[128];
    size_t body_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, mock_httpd_ws_get_frame_at(
        1, &type, body, sizeof(body), &body_len));
    body[body_len < sizeof(body) - 1 ? body_len : sizeof(body) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(HTTPD_WS_TYPE_TEXT, type);
    TEST_ASSERT_TRUE(strstr((const char *)body, "\"viewer_limit\"")
                     != NULL);

    /* The original session is untouched. */
    TEST_ASSERT_TRUE(ws_server_viewer_active());
}

/* ---------- S4: viewer close frees the slot ---------- */
TEST_CASE(
    "test_fw16_viewer_close_frees_slot [fw-16][server][scenario-S4]",
    "[ws-server][fw-16][close]")
{
    reset_state();
    server_up();

    TEST_ASSERT_EQUAL(ESP_OK, handshake(7));
    TEST_ASSERT_TRUE(ws_server_viewer_active());

    /* Simulate the socket vanishing (peer gone, RST): the fd
     * probe must report dead and free the slot. */
    mock_httpd_ws_kill_session(7);
    TEST_ASSERT_FALSE(ws_server_viewer_active());

    /* A new handshake on a fresh fd is accepted again — second
     * hello proves the accept path re-ran end-to-end. */
    TEST_ASSERT_EQUAL(ESP_OK, handshake(11));
    TEST_ASSERT_TRUE(ws_server_viewer_active());
    TEST_ASSERT_EQUAL_INT(2, mock_httpd_ws_send_call_count());
}

/* ---------- S5: status cadence while a viewer is connected ---------- */
TEST_CASE(
    "test_fw16_status_cadence_3_frames_in_90s [fw-16][status-cadence][scenario-S5]",
    "[ws-server][fw-16][status-cadence]")
{
    reset_state();
    server_up();

    TEST_ASSERT_EQUAL(ESP_OK, handshake(7));
    TEST_ASSERT_EQUAL_INT(1, mock_httpd_ws_send_call_count());

    /* Advance the periodic timer 90 s @ 30 s period → 3 fires. */
    void *timer_handle = ws_status_timer_handle_get();
    TEST_ASSERT_NOT_NULL(timer_handle);
    mock_esp_timer_advance_periodic(timer_handle, 90000);

    /* 1 hello + 3 status = 4 text frames total. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(4,
        mock_httpd_ws_send_call_count(),
        "expected 1 hello + 3 status frames after 90 s");

    for (size_t i = 1; i < 4; ++i) {
        char frame[256] = {0};
        int type = -1;
        size_t flen = 0;
        TEST_ASSERT_EQUAL(ESP_OK, mock_httpd_ws_get_frame_at(
            i, &type, (uint8_t *)frame, sizeof(frame) - 1, &flen));
        frame[flen] = '\0';
        TEST_ASSERT_EQUAL_INT(HTTPD_WS_TYPE_TEXT, type);
        char type_buf[16] = {0};
        TEST_ASSERT_TRUE(json_get_string(frame, "type",
            type_buf, sizeof(type_buf)));
        TEST_ASSERT_EQUAL_STRING("status", type_buf);
    }
}

/* ---------- S6: no emission after viewer close ---------- */
TEST_CASE(
    "test_fw16_no_status_after_viewer_close [fw-16][status-cadence][scenario-S6]",
    "[ws-server][fw-16][status-cadence]")
{
    reset_state();
    server_up();

    TEST_ASSERT_EQUAL(ESP_OK, handshake(7));
    TEST_ASSERT_EQUAL_INT(1, mock_httpd_ws_send_call_count());

    mock_httpd_ws_kill_session(7);

    void *timer_handle = ws_status_timer_handle_get();
    if (timer_handle) {
        mock_esp_timer_advance_periodic(timer_handle, 60000);
    }

    /* Only the pre-close hello; no status frames leaked out. */
    TEST_ASSERT_EQUAL_INT(1, mock_httpd_ws_send_call_count());
}
