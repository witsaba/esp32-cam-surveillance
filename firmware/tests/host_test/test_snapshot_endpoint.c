/* test_snapshot_endpoint.c — diagnostic GET /snapshot endpoint.
 *
 * The endpoint serves ONE real JPEG frame per request, pulled from
 * the depth-2 capture queue (capture_queue_receive_timeout) so the
 * R-16 single-caller invariant on esp_camera_fb_get stays intact —
 * the snapshot handler NEVER touches the driver directly.
 *
 * Purpose: bisect tool for the "no frames" investigation. If
 * /snapshot returns 200 image/jpeg, sensor + driver + queue are
 * alive and any streaming failure lives downstream (WS/network).
 * If it returns 503 no_frame, the camera hardware itself is not
 * producing frames — independent of WebSocket entirely.
 *
 * Scenarios:
 *   S1 — queued frame → 200, content-type image/jpeg, response
 *        bytes equal the queued frame buffer, consumer-owned
 *        esp_camera_fb_return called exactly once.
 *   S2 — empty queue → 503 status, JSON error body, no fb_return.
 *   S3 — IP-up listener registers BOTH /whoami and /snapshot.
 */

#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "capture.h"

#include "mock_esp_event.h"
#include "mock_esp_event_link.h"
#include "mock_http_server.h"
#include "mock_softap.h"
#include "mock_config.h"
#include "mock_esp_camera.h"

#include "softap.h"
#include "wifi_event.h"
#include "boot.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#ifdef UNITY_HOST_BUILD
#include "mock_supervision_record.h"
#include "mock_init_returns.h"
#endif

extern void softap_sta_listener_reset_for_test(void);
extern bool softap_sta_listener_is_active(void);

/* ---------- helpers ---------- */

static void reset_mocks(void)
{
    mock_httpd_reset();
    mock_esp_event_reset();
    mock_softap_reset();
    mock_config_reset();
    mock_esp_camera_reset();
    mock_supervision_reset();
    mock_init_returns_reset();
    capture_counters_reset_for_test();
    softap_sta_listener_reset_for_test();
}

/* Install the listener and fire IP-up so both URIs are registered
 * on the mock httpd registry. */
static void install_and_fire_ip_up(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, softap_sta_listener_install());
    mock_esp_event_fire_handler(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    TEST_ASSERT_TRUE(softap_sta_listener_is_active());
}

/* Fabricate a small "JPEG" frame buffer with recognizable content. */
static void fill_fake_fb(camera_fb_t *fb, uint8_t *storage, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        storage[i] = (uint8_t)(i & 0xFF);
    }
    storage[0] = 0xFF;
    storage[1] = 0xD8; /* SOI marker */
    fb->buf    = storage;
    fb->len    = len;
    fb->width  = 320;
    fb->height = 240;
    fb->format = 4; /* JPEG in esp32-camera enum terms */
}

/* ---------- scenarios ---------- */

TEST_CASE(
    "test_snapshot_queued_frame_served_as_jpeg [snapshot][scenario-S1]",
    "[softap][snapshot]")
{
    reset_mocks();

    /* Arm the module-static queue hooks exactly like a real boot. */
    TEST_ASSERT_EQUAL(ESP_OK, capture_task_start());

    install_and_fire_ip_up();

    /* Queue one frame — the same path the capture task uses. */
    uint8_t storage[64];
    camera_fb_t fb;
    fill_fake_fb(&fb, storage, sizeof(storage));
    TEST_ASSERT_TRUE(capture_queue_send_drop_on_full(
        capture_queue_for_test(), &fb));

    mock_httpd_req_t *req = mock_httpd_req_new();

    int returns_before = mock_esp_camera_fb_return_call_count();
    esp_err_t rc = mock_httpd_invoke_registered_handler(
        "/snapshot", HTTP_GET, req);

    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
    TEST_ASSERT_NOT_NULL(req->captured_content_type);
    TEST_ASSERT_EQUAL_STRING("image/jpeg", req->captured_content_type);
    TEST_ASSERT_EQUAL_UINT32(sizeof(storage), req->captured_response_len);
    TEST_ASSERT_EQUAL_MEMORY(storage, req->captured_response_buffer,
                             sizeof(storage));

    /* Consumer owns the buffer: exactly one fb_return, after send. */
    TEST_ASSERT_EQUAL_INT(1,
        mock_esp_camera_fb_return_call_count() - returns_before);

    mock_httpd_req_free(req);
}

TEST_CASE(
    "test_snapshot_empty_queue_returns_503 [snapshot][scenario-S2]",
    "[softap][snapshot]")
{
    reset_mocks();

    TEST_ASSERT_EQUAL(ESP_OK, capture_task_start());
    install_and_fire_ip_up();

    /* No frame queued — bounded wait must expire into a clean 503. */
    mock_httpd_req_t *req = mock_httpd_req_new();

    int returns_before = mock_esp_camera_fb_return_call_count();
    esp_err_t rc = mock_httpd_invoke_registered_handler(
        "/snapshot", HTTP_GET, req);

    TEST_ASSERT_NOT_EQUAL(ESP_OK, rc);
    TEST_ASSERT_EQUAL_INT(503, req->captured_status);
    TEST_ASSERT_NULL(req->captured_content_type);
    TEST_ASSERT_NOT_NULL(req->captured_response_buffer);
    TEST_ASSERT_TRUE(strstr(req->captured_response_buffer,
                            "no_frame") != NULL);

    /* Nothing was consumed from the driver — no fb_return. */
    TEST_ASSERT_EQUAL_INT(0,
        mock_esp_camera_fb_return_call_count() - returns_before);

    mock_httpd_req_free(req);
}

TEST_CASE(
    "test_snapshot_listener_registers_both_uris [snapshot][scenario-S3]",
    "[softap][snapshot]")
{
    reset_mocks();

    install_and_fire_ip_up();

    /* Two registrations total: /whoami (FW-05.5) + /snapshot.
     * The FW-16 /cams WebSocket endpoint is attached by the ws
     * component on its own IP-up subscription. */
    TEST_ASSERT_EQUAL_INT(2, mock_httpd_register_uri_handler_call_count());

    /* Most recently registered is /snapshot… */
    const char *uri = NULL;
    int          method = -1;
    mock_httpd_last_registered_uri(&uri, &method);
    TEST_ASSERT_NOT_NULL(uri);
    TEST_ASSERT_EQUAL_STRING("/snapshot", uri);
    TEST_ASSERT_EQUAL_INT(HTTP_GET, method);

    /* …and /whoami is still dispatchable through the registry.
     * The invoke result is the HANDLER's own return (whoami needs
     * identity priming we don't do here); what matters is that it
     * is NOT ESP_ERR_NOT_FOUND, i.e. the URI is registered. */
    mock_httpd_req_t *req = mock_httpd_req_new();
    esp_err_t whoami_rc =
        mock_httpd_invoke_registered_handler("/whoami", HTTP_GET, req);
    TEST_ASSERT_NOT_EQUAL(ESP_ERR_NOT_FOUND, whoami_rc);
    mock_httpd_req_free(req);
}
