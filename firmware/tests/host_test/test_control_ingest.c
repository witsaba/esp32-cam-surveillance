/* test_control_ingest.c — FW-18 RX-seam suite (U3).
 *
 *   M1  mock self-test: a primed WS frame drains its primed
 *       type/payload through the two-call IDF recv semantics
 *       (size probe len=0 → total+type; fill → bounded copy).
 *
 *   S1  a TEXT command at the seam is ENQUEUED on the control
 *       ring, not processed inline (spec #3970 FW-18.2).
 *   S2  the seam consumes the TEXT frame from the connection and
 *       the handler answers ESP_OK — post-handshake HTTP NEVER
 *       fails (error swallowing, D5).
 *   S3  non-TEXT frames are silently ignored — no enqueue, no
 *       drop-counter bump, no reply (ruling #3966.3) — and the
 *       stream keeps working: the NEXT valid frame still parses.
 *   S4  ten primed commands against the depth-8 ring: exactly 8
 *       enqueued FIFO + 2 dropped NEWEST on the SAME counter
 *       (ruling #3966.2).
 *   S5  an oversize >512 B TEXT frame: dropped on the SAME
 *       counter, chunk-drained so the socket stays synced — the
 *       following valid frame still parses (D5).
 *
 * Driver: softap_sta_listener_install() + mock IP-up event start
 * the httpd mock registry and attach /cams (mirrors
 * test_ws_server.c); data frames are delivered through
 * mock_httpd_invoke_registered_handler with req->method set off
 * HTTP_GET to model the upgraded post-handshake data phase.
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

#include "control.h"
#include "config.h"
#include "softap.h"
#include "ws.h"
#include "ws_server.h"

#include "mock_esp_event.h"
#include "mock_esp_event_link.h"   /* IP_EVENT base */
#include "mock_http_server.h"
#include "mock_esp_timer.h"
#include "mock_esp_system.h"
#include "mock_nvs_flash_link.h"
#include "mock_init_returns.h"
#include "esp_event.h"

extern void softap_sta_listener_reset_for_test(void);
extern void ws_status_timer_reset_handle_for_test(void);

static void ingest_reset(void)
{
    mock_httpd_reset();
    mock_esp_event_reset();
    mock_init_returns_reset();
    mock_esp_timer_reset();
    softap_sta_listener_reset_for_test();
    ws_server_reset_for_test();
    ws_status_timer_reset_handle_for_test();
    control_reset_for_test();

    uint8_t mac[6] = {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    mock_esp_read_mac_set_bytes(mac);
}

static void server_up(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, softap_sta_listener_install());
    TEST_ASSERT_EQUAL(ESP_OK, ws_server_install());
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_esp_event_fire_handler(IP_EVENT,
                                    IP_EVENT_STA_GOT_IP,
                                    NULL));
}

/* Deliver ONE post-handshake data-frame cycle against /cams. The
 * registry lookup runs on the original GET registration while
 * req->method != HTTP_GET drives the (else) ingest branch — the
 * same discriminator the production handler uses. */
static esp_err_t deliver_data_frame(mock_httpd_req_t *req)
{
    req->method = HTTP_POST;
    return mock_httpd_invoke_registered_handler(
        CONFIG_FIRMWARE_WS_PATH, HTTP_GET, req);
}

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

/* ---------- S1: TEXT command is enqueued, NOT processed inline --- */
TEST_CASE(
    "test_ingest_text_command_enqueued_not_processed_inline [fw-18.2]",
    "[control-ingest][fw-18.2]")
{
    ingest_reset();
    server_up();

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);
    const char *cmd = "{\"cmd\":\"reboot\",\"id\":1}";
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_req_prime_ws_frame(req, HTTPD_WS_TYPE_TEXT,
                                      cmd, strlen(cmd)));

    TEST_ASSERT_EQUAL(ESP_OK, deliver_data_frame(req));

    /* ENQUEUED: exactly one queued heap copy holding the verbatim
     * command — an inline processor would have consumed it before
     * any queue observation could see it. */
    TEST_ASSERT_EQUAL_INT(1, control_queue_for_test()->count);
    void *p = NULL;
    TEST_ASSERT_TRUE(control_queue_receive_timeout(&p, 0));
    TEST_ASSERT_EQUAL_STRING(cmd, (const char *)p);
    free(p);

    /* Nothing was dropped getting it there. */
    TEST_ASSERT_EQUAL_UINT32(0, control_frames_dropped_get());

    mock_httpd_req_free(req);
}

/* ---------- S2: frame consumed at the seam, handler ESP_OK ------ */
TEST_CASE(
    "test_ingest_text_consumed_handler_ok [fw-18.2][seam]",
    "[control-ingest][fw-18.2]")
{
    ingest_reset();
    server_up();

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);
    const char *cmd = "{\"cmd\":\"identify\",\"id\":42}";
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_req_prime_ws_frame(req, HTTPD_WS_TYPE_TEXT,
                                      cmd, strlen(cmd)));

    /* Error swallowing: the handler NEVER fails post-handshake. */
    TEST_ASSERT_EQUAL(ESP_OK, deliver_data_frame(req));

    /* The TEXT frame was consumed off the connection... */
    TEST_ASSERT_EQUAL_INT(0,
        (int)mock_httpd_req_ws_frames_pending(req));
    /* ...and handed over as exactly one queued copy. */
    TEST_ASSERT_EQUAL_INT(1, control_queue_for_test()->count);

    mock_httpd_req_free(req);
}

/* ---------- S3: non-TEXT silently ignored, stream unaffected ---- */
TEST_CASE(
    "test_ingest_non_text_silent_ignore [fw-18.2][ruling-3]",
    "[control-ingest][fw-18.2]")
{
    ingest_reset();
    server_up();

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);

    /* A BINARY frame (and a PING): neither may touch the control
     * state — no enqueue, no drop accounting, no reply. */
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_req_prime_ws_frame(req, HTTPD_WS_TYPE_BINARY,
                                      "\x00\x01\x02", 3));
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_req_prime_ws_frame(req, HTTPD_WS_TYPE_PING,
                                      NULL, 0));

    TEST_ASSERT_EQUAL(ESP_OK, deliver_data_frame(req));
    TEST_ASSERT_EQUAL(ESP_OK, deliver_data_frame(req));

    /* SILENT: zero control-module state change. */
    TEST_ASSERT_EQUAL_INT(0, control_queue_for_test()->count);
    TEST_ASSERT_EQUAL_UINT32(0, control_frames_dropped_get());

    /* The stream still works: the next valid TEXT frame parses. */
    const char *cmd = "{\"cmd\":\"sleep\",\"id\":9}";
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_req_prime_ws_frame(req, HTTPD_WS_TYPE_TEXT,
                                      cmd, strlen(cmd)));
    TEST_ASSERT_EQUAL(ESP_OK, deliver_data_frame(req));
    TEST_ASSERT_EQUAL_INT(1, control_queue_for_test()->count);

    void *p = NULL;
    TEST_ASSERT_TRUE(control_queue_receive_timeout(&p, 0));
    TEST_ASSERT_EQUAL_STRING(cmd, (const char *)p);
    free(p);

    mock_httpd_req_free(req);
}

/* ---------- S4: ten primed → 8 enqueued FIFO + 2 dropped -------- */
TEST_CASE(
    "test_ingest_ten_primed_eight_enqueued_two_dropped [fw-18.2][ruling-2]",
    "[control-ingest][fw-18.2]")
{
    ingest_reset();
    server_up();

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);

    char body[64];
    for (int i = 0; i < 10; ++i) {
        snprintf(body, sizeof(body),
                 "{\"cmd\":\"frobnicate_%d\",\"id\":\"i%d\"}", i, i);
        TEST_ASSERT_EQUAL(ESP_OK,
            mock_httpd_req_prime_ws_frame(req, HTTPD_WS_TYPE_TEXT,
                                          body, strlen(body)));
    }
    TEST_ASSERT_EQUAL_INT(10,
        (int)mock_httpd_req_ws_frames_pending(req));

    /* One handler invocation per received frame (IDF contract). */
    for (int i = 0; i < 10; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, deliver_data_frame(req));
    }
    TEST_ASSERT_EQUAL_INT(0,
        (int)mock_httpd_req_ws_frames_pending(req));

    /* Depth-8 ring full; the two NEWEST frames dropped NEWEST on
     * the same drop counter — nothing blocked, nothing stalled. */
    TEST_ASSERT_EQUAL_INT(CONTROL_QUEUE_DEPTH,
                          control_queue_for_test()->count);
    TEST_ASSERT_EQUAL_UINT32(2, control_frames_dropped_get());

    /* FIFO preserved: queued ids are i0..i7 in arrival order; the
     * dropped i8/i9 appear nowhere. */
    for (int i = 0; i < CONTROL_QUEUE_DEPTH; ++i) {
        void *p = NULL;
        TEST_ASSERT_TRUE_MESSAGE(control_queue_receive_timeout(&p, 0),
                                 "FIFO drain");
        snprintf(body, sizeof(body), "\"id\":\"i%d\"", i);
        TEST_ASSERT_TRUE_MESSAGE(
            strstr((const char *)p, body) != NULL,
            "queued frame out of order");
        free(p);
    }

    mock_httpd_req_free(req);
}

/* ---------- S5: oversize >512 B dropped, stream stays synced ---- */
TEST_CASE(
    "test_ingest_oversize_dropped_same_counter_stream_synced [fw-18.3][d5]",
    "[control-ingest][fw-18.3]")
{
    ingest_reset();
    server_up();

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);

    /* One 600 B TEXT frame (> CONTROL_FRAME_MAX 512) followed by
     * a valid small command on the SAME connection. */
    char big[601];
    memset(big, 'a', sizeof(big)); /* content never parsed */
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_req_prime_ws_frame(req, HTTPD_WS_TYPE_TEXT,
                                      big, sizeof(big)));
    const char *cmd = "{\"cmd\":\"identify\",\"id\":7}";
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_req_prime_ws_frame(req, HTTPD_WS_TYPE_TEXT,
                                      cmd, strlen(cmd)));

    TEST_ASSERT_EQUAL(ESP_OK, deliver_data_frame(req)); /* oversize */
    TEST_ASSERT_EQUAL(ESP_OK, deliver_data_frame(req)); /* valid    */

    /* Dropped on the SAME counter (no separate oversize bucket),
     * and the drain kept the stream synced: the NEXT frame parsed
     * instead of reading mid-frame garbage. */
    TEST_ASSERT_EQUAL_UINT32(1, control_frames_dropped_get());
    TEST_ASSERT_EQUAL_INT(1, control_queue_for_test()->count);
    void *p = NULL;
    TEST_ASSERT_TRUE(control_queue_receive_timeout(&p, 0));
    TEST_ASSERT_EQUAL_STRING(cmd, (const char *)p);
    free(p);
    TEST_ASSERT_EQUAL_INT(0,
        (int)mock_httpd_req_ws_frames_pending(req));

    mock_httpd_req_free(req);
}
