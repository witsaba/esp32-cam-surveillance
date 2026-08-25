/* test_control_ingest.c — FW-18 RX-seam suite (U3).
 *
 *   M1  mock self-test: a primed WS frame drains its primed
 *       type/payload through the two-call IDF recv semantics
 *       (size probe len=0 → total+type; fill → bounded copy).
 *
 * Seam scenarios land with T3.3 (S1–S5) once the mock recv seam
 * is GREEN.
 */
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "mock_http_server.h"

/* ---------- M1: primed frame drains primed type/payload ---------- */
TEST_CASE(
    "test_ingest_mock_primed_frame_drains_type_and_payload [fw-18][mock-self]",
    "[control-ingest][fw-18][mock]")
{
    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);

    const char *body = "{\"cmd\":\"identify\",\"id\":7}";
    size_t body_len = strlen(body);
    TEST_ASSERT_EQUAL(ESP_OK, mock_httpd_req_prime_ws_frame(
                                  req, HTTPD_WS_TYPE_TEXT,
                                  body, body_len));
    /* Exactly one frame pending after one prime. */
    TEST_ASSERT_EQUAL_INT(1,
        (int)mock_httpd_req_ws_frames_pending(req));

    /* Two-call IDF semantics — call 1: size/type probe (len=0). */
    httpd_ws_frame_t pkt = {0};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_ws_recv_frame(req, &pkt, 0));
    TEST_ASSERT_EQUAL_INT((int)body_len, (int)pkt.len);
    TEST_ASSERT_EQUAL_INT(HTTPD_WS_TYPE_TEXT, (int)pkt.type);

    /* Call 2: fill up to max_len — payload bytes match the prime. */
    uint8_t buf[128] = {0};
    pkt.payload = buf;
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_ws_recv_frame(req, &pkt, pkt.len));
    TEST_ASSERT_EQUAL_INT((int)body_len, (int)pkt.len);
    TEST_ASSERT_EQUAL_STRING(body, (const char *)buf);

    /* Drained to empty: the FIFO popped the consumed frame. */
    TEST_ASSERT_EQUAL_INT(0,
        (int)mock_httpd_req_ws_frames_pending(req));

    mock_httpd_req_free(req);
}
