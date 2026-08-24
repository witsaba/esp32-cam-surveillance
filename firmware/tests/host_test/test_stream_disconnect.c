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
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "capture.h"
#include "stream.h"
#include "stream_fragment.h"
#include "ws.h"
#include "config.h"
#include "boot.h"
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_supervision_record.h"
#include "mock_init_returns.h"
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
