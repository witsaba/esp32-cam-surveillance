/* test_stream_disconnect.c — FW-15.3/15.4 + FW-16 host scenarios
 * for the stream task loop under a DEAD socket (REQ-ST-005 /
 * REQ-ST-007, design D4).
 *
 * Disconnect semantics = drain + drop + count:
 *   - any send failure aborts the frame's send attempt,
 *   - the fb is returned to the driver EXACTLY ONCE (success OR
 *     failure — consumer-owned post-receive, REQ-ST-005),
 *   - the stream drop counter increments,
 *   - the consume loop CONTINUES so the producer never blocks on
 *     a full queue (the producer's own drop-on-full absorbs
 *     overflow during the dead window).
 *
 * Scenarios:
 *   S1 (REQ-ST-005) — failed send still returns the fb exactly once.
 *   S2 (REQ-ST-007) — 4 queued frames against a dead socket: all 4
 *       are consumed (drained), each fb_returned exactly once,
 *       dropped == 4, sent == 0; a subsequent receive times out
 *       bounded (queue empty — never blocked forever).
 *   S3 (REQ-ST-001/002 happy path through the LOOP) — connected
 *       viewer: one iteration sends one complete binary message and
 *       bumps `sent`; the greppable per-frame log line is emitted from
 *       the send path (device-log evidence seam, REQ-ST-008/009).
 *   S4 (FW-16) — NO viewer sink installed: every frame drops with
 *       the same D4 accounting (drop-count when no viewer).
 *   S5 (FW-19, ruling 4) — mid-stream disconnect: kill_session +
 *       failed send ⇒ viewer_clear raises the capture auto-stop
 *       word; the gate halts within one loop tick and acquires
 *       nothing afterwards.
 *   S6 (FW-19 triangulation) — same disconnect trigger while the
 *       gate is STOPPED (fresh boot): the word fires once but the
 *       gate never opens — zero acquisitions either way.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "capture.h"
#include "stream.h"
#include "stream_fragment.h"
#include "ws.h"
#include "ws_server.h"
#include "config.h"
#include "boot.h"
#include "softap.h"
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_supervision_record.h"
#include "mock_init_returns.h"
#include "mock_esp_event.h"
#include "mock_esp_event_link.h"
#include "mock_esp_system.h"
#include "mock_http_server.h"
#include "mock_softap.h"
#include "ws_sink_recorder.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* Full fixture: fresh mocks + armed capture-queue hooks + ws_init
 * module state + (optionally) a recorder viewer sink. */
static void stream_loop_with_mocks(bool install_viewer)
{
    mock_esp_camera_reset();
    mock_supervision_reset();
    mock_init_returns_reset();
    capture_counters_reset_for_test();
    stream_counters_reset_for_test();

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.wifi.ssid, "TestSSID", sizeof(cfg.wifi.ssid) - 1);
    TEST_ASSERT_EQUAL(ESP_OK, ws_init(&cfg));

    if (install_viewer) {
        ws_sink_recorder_reset();
        TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_install());
    } else {
        /* No viewer: leave the disconnected stubs ws_install set
         * during ws_init in place. */
        ws_sink_recorder_reset();
        ws_sink_install(NULL);
    }
    TEST_ASSERT_EQUAL(ESP_OK, capture_task_start()); /* arms queue hooks */
}

static camera_fb_t make_fb(uint8_t *storage, size_t len)
{
    camera_fb_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.buf = storage;
    fb.len = len;
    return fb;
}

/* ---------- S1 — REQ-ST-005: failed send returns fb exactly once ---------- */
TEST_CASE(
    "test_fw15_failed_send_returns_fb_exactly_once [fw-15.3][req-st-005][scenario-S1]",
    "[stream][fw-15.3][fb-return]")
{
    stream_loop_with_mocks(true);

    /* Dead socket: every send fails. */
    ws_sink_recorder_fail_all_set(true);

    static uint8_t storage[8000];
    camera_fb_t fb = make_fb(storage, sizeof(storage));
    TEST_ASSERT_TRUE(capture_queue_send_drop_on_full(
        capture_queue_for_test(), &fb));

    /* One loop iteration: consume → attempt send → FAIL. */
    TEST_ASSERT_FALSE(stream_loop_iteration());

    /* No leak: the buffer went back to the driver exactly once
     * despite the failed send. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_camera_fb_return_call_count());
    /* Counted as dropped, not sent. */
    TEST_ASSERT_EQUAL_UINT32(0, stream_frames_sent_get());
    TEST_ASSERT_EQUAL_UINT32(1, stream_frames_dropped_get());
}

/* ---------- S2 — REQ-ST-007: dead socket keeps draining ---------- */
TEST_CASE(
    "test_fw15_dead_socket_drains_all_frames [fw-15.3][req-st-007][scenario-S2]",
    "[stream][fw-15.3][drain]")
{
    stream_loop_with_mocks(true);
    ws_sink_recorder_fail_all_set(true); /* persistent dead socket */

    /* The producer side enqueues frames back-to-back while the
     * dead-socket consumer keeps draining. Queue depth is 2, so
     * alternate push/consume rounds: if the consumer ever wedged,
     * round 2's pushes would fail with a full queue. */
    static uint8_t s1[100], s2[200], s3[300], s4[400];
    camera_fb_t fbs[4] = {
        make_fb(s1, sizeof(s1)),
        make_fb(s2, sizeof(s2)),
        make_fb(s3, sizeof(s3)),
        make_fb(s4, sizeof(s4)),
    };
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            capture_queue_send_drop_on_full(capture_queue_for_test(),
                                            &fbs[i]),
            "producer must never block — consumer keeps draining");
        TEST_ASSERT_FALSE_MESSAGE(stream_loop_iteration(),
            "iteration reports failure but must NOT wedge");
        TEST_ASSERT_EQUAL_INT(i + 1, mock_esp_camera_fb_return_call_count());
    }
    TEST_ASSERT_EQUAL_UINT32(0, stream_frames_sent_get());
    TEST_ASSERT_EQUAL_UINT32(4, stream_frames_dropped_get());

    /* Queue is empty now — a further receive times out BOUNDED
     * instead of blocking forever. */
    void *out = NULL;
    TEST_ASSERT_FALSE(capture_queue_receive_timeout(&out, 20));
}

/* ---------- S3 — happy path through the loop + counters ---------- */
TEST_CASE(
    "test_fw15_healthy_socket_loop_sends_and_counts [fw-15.3][req-st-002][scenario-S3]",
    "[stream][fw-15.3][happy-path]")
{
    stream_loop_with_mocks(true);

    static uint8_t storage[4096];
    for (size_t i = 0; i < sizeof(storage); i++) storage[i] = (uint8_t)i;
    camera_fb_t fb = make_fb(storage, sizeof(storage));
    TEST_ASSERT_TRUE(capture_queue_send_drop_on_full(
        capture_queue_for_test(), &fb));

    TEST_ASSERT_TRUE(stream_loop_iteration());

    /* Exactly one complete binary frame, byte-exact payload. */
    TEST_ASSERT_EQUAL_size_t(1, ws_sink_recorder_bin_count());
    static uint8_t out[24576];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_get_bin_at(
        0, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL_size_t(sizeof(storage), out_len);
    TEST_ASSERT_EQUAL_MEMORY(storage, out, sizeof(storage));

    /* Buffer still returned exactly once after SUCCESS. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_camera_fb_return_call_count());
    TEST_ASSERT_EQUAL_UINT32(1, stream_frames_sent_get());
    TEST_ASSERT_EQUAL_UINT32(0, stream_frames_dropped_get());
}

/* ---------- S4 — FW-16: no viewer → drop-count keeps draining ---------- */
TEST_CASE(
    "test_fw16_no_viewer_frame_dropped_and_counted [fw-16][server][scenario-S4]",
    "[stream][fw-16][no-viewer]")
{
    /* No sink installed — ws_init left the disconnected stubs
     * active, exactly like a booted device before any /cams
     * handshake. */
    stream_loop_with_mocks(false);

    static uint8_t storage[3000];
    camera_fb_t fb = make_fb(storage, sizeof(storage));
    TEST_ASSERT_TRUE(capture_queue_send_drop_on_full(
        capture_queue_for_test(), &fb));

    /* Iteration consumes + returns the fb but reports failure. */
    TEST_ASSERT_FALSE(stream_loop_iteration());

    /* D4 accounting identical to a dead socket: fb returned once,
     * dropped++ (not sent). Nothing reached any sink. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_camera_fb_return_call_count());
    TEST_ASSERT_EQUAL_size_t(0, ws_sink_recorder_bin_count());
    TEST_ASSERT_EQUAL_UINT32(0, stream_frames_sent_get());
    TEST_ASSERT_EQUAL_UINT32(1, stream_frames_dropped_get());

    /* The queue drained — producer never wedges while idle. */
    void *out = NULL;
    TEST_ASSERT_FALSE(capture_queue_receive_timeout(&out, 20));
}

/* ---------- FW-19 server-mode fixture (S5/S6) ----------
 *
 * Brings the REAL /cams endpoint up through the IP-up path and
 * completes one handshake so the httpd-backed SERVER sink is
 * installed — exactly the production wiring a mid-stream
 * disconnect travels (design D6: forced async-send failure while
 * the fd probe stays alive). Mirrors test_ws_server.c's
 * reset/server_up/handshake helpers.
 */
extern void softap_sta_listener_reset_for_test(void);
extern void ws_status_timer_reset_handle_for_test(void);

static void server_stream_fixture(void)
{
    mock_esp_camera_reset();
    mock_supervision_reset();
    mock_init_returns_reset();
    mock_httpd_reset();
    mock_esp_event_reset();
    mock_softap_reset();
    softap_sta_listener_reset_for_test();
    ws_server_reset_for_test();
    ws_status_timer_reset_handle_for_test();
    capture_counters_reset_for_test();
    stream_counters_reset_for_test();
    capture_gate_reset_for_test();
    ws_sink_recorder_reset();

    uint8_t mac[6] = {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    mock_esp_read_mac_set_bytes(mac);

    TEST_ASSERT_EQUAL(ESP_OK, softap_sta_listener_install());
    TEST_ASSERT_EQUAL(ESP_OK, ws_server_install());
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_esp_event_fire_handler(IP_EVENT,
                                    IP_EVENT_STA_GOT_IP,
                                    NULL));
    TEST_ASSERT_EQUAL(ESP_OK, capture_task_start());

    /* Handshake on fd 7 → viewer_accept captures fd + installs
     * the server sink (hello may or may not lead the ring; the
     * disconnect scenarios below don't depend on it). */
    mock_httpd_req_t *req = mock_httpd_req_new();
    req->method = HTTP_GET;
    req->sockfd = 7;
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_invoke_registered_handler(
            CONFIG_FIRMWARE_WS_PATH, HTTP_GET, req));
    mock_httpd_req_free(req);
}

/* ---------- S5 — FW-19: mid-stream disconnect auto-stops ---------- */
TEST_CASE(
    "test_fw19_midstream_disconnect_auto_stops_capture [fw-19][server][scenario-S5]",
    "[stream][fw-19][auto-stop]")
{
    server_stream_fixture();
    TEST_ASSERT_TRUE(ws_server_viewer_active());

    /* Viewer issued stream.on: gate open @10 fps. */
    capture_run_start(10);

    capture_counters_t counters = {0};
    capture_queue_t *q = capture_queue_for_test();

    /* Gate OPEN pre-disconnect: two paced steps acquire two frames. */
    for (int i = 0; i < 2; i++) {
        capture_gate_in_t in = {
            .gate_open   = capture_running_get(),
            .fps_applied = 10,
        };
        capture_gate_out_t out = {0};
        capture_gated_iteration(q, &counters, &in, &out);
        TEST_ASSERT_TRUE(out.ran);
        TEST_ASSERT_FALSE(out.stop_latched);
    }
    int gets_streaming = mock_esp_camera_fb_get_call_count();
    TEST_ASSERT_TRUE(gets_streaming >= 2);

    /* Mid-stream disconnect: the socket dies MID-SEND. The forced
     * send-failure injection (mock_httpd_ws_fail_sends_set,
     * design D6) keeps the fd probe ALIVE while writes fail —
     * exactly the device race — so this text emission (e.g. the
     * 30 s status frame) runs server_sink_send_text ⇒ async send
     * FAILS ⇒ viewer_clear ⇒ capture_auto_stop_request(). The
     * path completes synchronously: the httpd-worker context is
     * never blocked by the hook (one relaxed store). */
    mock_httpd_ws_fail_sends_set(true);
    const char status_ping[] = "{\"type\":\"status\"}";
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ws_sink_send_text(status_ping,
                                        sizeof(status_ping) - 1));
    /* viewer_clear also freed the slot + uninstalled the sink. */
    TEST_ASSERT_FALSE(ws_server_viewer_active());

    /* Stop word PENDING on the wire — the capture loop hasn't
     * ticked since. The wrapper's exchange-before-step consumes
     * it EXACTLY ONCE (no lost wakeup, design D5). */
    TEST_ASSERT_TRUE(capture_running_get());
    TEST_ASSERT_TRUE(capture_stop_request_take());
    TEST_ASSERT_FALSE(capture_stop_request_take());

    /* Next wrapper tick latches: gate closes with NO acquisition
     * on the latching step (the wrapper then calls run_stop). */
    capture_gate_in_t in = {
        .gate_open      = true,
        .fps_applied    = 10,
        .stop_requested = true,
    };
    capture_gate_out_t out = {0};
    capture_gated_iteration(q, &counters, &in, &out);
    TEST_ASSERT_TRUE(out.stop_latched);
    TEST_ASSERT_FALSE(out.ran);
    capture_run_stop();

    /* Halt ≤1 simulated second: ten idle ticks @100 ms — ZERO
     * acquisitions after the disconnect. */
    int gets_at_halt = mock_esp_camera_fb_get_call_count();
    for (int i = 0; i < 10; i++) {
        capture_gate_in_t idle = {
            .gate_open = capture_running_get(),
        };
        capture_gate_out_t o = {0};
        capture_gated_iteration(q, &counters, &idle, &o);
        TEST_ASSERT_FALSE(o.ran);
    }
    TEST_ASSERT_EQUAL_INT(gets_at_halt,
                          mock_esp_camera_fb_get_call_count());
    TEST_ASSERT_FALSE(capture_running_get());

    /* Fixture teardown: leave the shared single-binary world as
     * later suites expect (the mock event-subscription table is
     * finite; leftover /cams + listener subscriptions would
     * starve their ws_init calls). */
    mock_httpd_ws_fail_sends_set(false);
    mock_httpd_reset();
    mock_esp_event_reset();
    softap_sta_listener_reset_for_test();
    ws_server_reset_for_test();
}

/* ---------- S6 — FW-19 triangulation: stopped gate stays stopped ---------- */
TEST_CASE(
    "test_fw19_disconnect_while_stopped_never_opens_gate [fw-19][server][scenario-S6]",
    "[stream][fw-19][auto-stop]")
{
    /* Fresh boot world: viewer handshakes but never issues
     * stream.on — the gate is STOPPED (FW-19 default). */
    server_stream_fixture();
    TEST_ASSERT_TRUE(ws_server_viewer_active());
    TEST_ASSERT_FALSE(capture_running_get());

    /* Same trigger as S5: forced mid-send failure ⇒ viewer_clear
     * ⇒ auto-stop word. Harmless while stopped. */
    mock_httpd_ws_fail_sends_set(true);
    const char status_ping[] = "{\"type\":\"status\"}";
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ws_sink_send_text(status_ping,
                                        sizeof(status_ping) - 1));
    TEST_ASSERT_FALSE(ws_server_viewer_active());

    /* Word fired once… */
    TEST_ASSERT_TRUE(capture_stop_request_take());
    TEST_ASSERT_FALSE(capture_stop_request_take());

    /* …but ten idle ticks later the gate NEVER opened and the
     * driver was NEVER touched (R-25 invariant intact). */
    capture_counters_t counters = {0};
    for (int i = 0; i < 10; i++) {
        capture_gate_in_t in = {
            .gate_open = capture_running_get(),
        };
        capture_gate_out_t out = {0};
        capture_gated_iteration(capture_queue_for_test(), &counters,
                                &in, &out);
        TEST_ASSERT_FALSE(out.ran);
        TEST_ASSERT_FALSE(out.stop_latched);
    }
    TEST_ASSERT_EQUAL_INT(0, mock_esp_camera_fb_get_call_count());
    TEST_ASSERT_FALSE(capture_running_get());

    /* Fixture teardown — same rationale as S5. */
    mock_httpd_ws_fail_sends_set(false);
    mock_httpd_reset();
    mock_esp_event_reset();
    softap_sta_listener_reset_for_test();
    ws_server_reset_for_test();
}
