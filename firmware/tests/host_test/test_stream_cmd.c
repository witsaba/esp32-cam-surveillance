/* test_stream_cmd.c — FW-19 stream-command handler suite (U2).
 *
 * Drives the D3/D4 handler through the REAL router
 * (control_frame_process → registered stream_cmd_handle):
 *   S:Registered dispatch retires not_implemented · S:Default
 *   absent fps echoes CONFIG 5 · S:Echo explicit fps echoed as
 *   applied · S:Clamp range clamps silently (ruling 6) ·
 *   S:BadField wrong-typed on/fps, id lossless · S:NoViewer
 *   start refusal, gate stays stopped (ruling 7) · S:Ordering
 *   ack precedes frame data (D3 pin) · S:StopHalt halt ≤1 sim-s,
 *   single current-fps ack, quiet after.
 *
 * Driver: softap_sta_listener_install() + mock IP-up event attach
 * /cams (mirrors test_ws_server.c); handshakes run through
 * mock_httpd_invoke_registered_handler. Ack envelopes are read
 * from ws_sink_recorder text slots (the fd-probe-based viewer
 * guard is independent of the installed sink, so the recorder
 * never masks viewer state). The ordering test reads the httpd
 * mock WS ring instead — it is the only surface preserving
 * cross-type (text vs binary) arrival ORDER.
 */
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <string.h>

#include "esp_err.h"

#include "control.h"
#include "stream_cmd.h"
#include "capture.h"
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
#include "mock_esp_camera.h"
#include "ws_sink_recorder.h"
#include "esp_event.h"

extern void softap_sta_listener_reset_for_test(void);
extern void ws_status_timer_reset_handle_for_test(void);

/* Fresh-boot state for every test + PRODUCTION registration (the
 * call under test mirrors boot.c's one-liner). Idempotent: the
 * registry store repeats harmlessly. */
static void stream_reset(void)
{
    mock_httpd_reset();
    mock_esp_event_reset();
    mock_init_returns_reset();
    mock_esp_timer_reset();
    softap_sta_listener_reset_for_test();
    ws_server_reset_for_test();
    ws_status_timer_reset_handle_for_test();
    control_reset_for_test();
    capture_counters_reset_for_test();
    capture_gate_reset_for_test();
    mock_esp_camera_reset();

    uint8_t mac[6] = {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    mock_esp_read_mac_set_bytes(mac);
    mock_nvs_seed_str("config", "name", "TestCam");

    /* Recorder slots are module-static — clear them so ack
     * assertions always read THIS test's envelopes (the sink
     * itself gets reinstalled by ws_server_reset_for_test). */
    ws_sink_recorder_reset();

    stream_cmd_register();
}

/* Bring the STA listener + /cams up through the real IP-up path,
 * accept THE viewer on fd 7, then swap in the recorder sink so
 * ack text frames land in inspectable slots (hello stays on the
 * httpd ring — it precedes the swap). */
static void viewer_join(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, softap_sta_listener_install());
    TEST_ASSERT_EQUAL(ESP_OK, ws_server_install());
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_esp_event_fire_handler(IP_EVENT,
                                    IP_EVENT_STA_GOT_IP,
                                    NULL));

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);
    req->method = HTTP_GET;
    req->sockfd = 7;
    TEST_ASSERT_EQUAL(ESP_OK, mock_httpd_invoke_registered_handler(
                                  CONFIG_FIRMWARE_WS_PATH,
                                  HTTP_GET, req));
    mock_httpd_req_free(req);
    TEST_ASSERT_TRUE(ws_server_viewer_active());

    TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_install());
}

/* Drive one body through the REAL router with the production
 * handler registered. Dispatched commands emit nothing here —
 * the handler owns the wire (returns 0). */
static void dispatch(const char *body)
{
    char sink[CONTROL_FRAME_MAX];
    size_t n = control_frame_process(body, strlen(body),
                                     sink, sizeof(sink) - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, (int)n, "dispatched commands emit no router envelope");
}

/* Recorder text slot reader — NUL-terminates for byte-exact
 * comparison. */
static void ack_text_at(size_t idx, char *out, size_t cap)
{
    TEST_ASSERT_EQUAL(ESP_OK,
        ws_sink_recorder_get_text_at(idx, out, cap));
}

/* Step `n` discrete ticks through the PURE gated seam with ZERO
 * sleeps; pins the exact pacing each tick reports (design D2). */
static void step_ticks(bool open, uint32_t fps, int n)
{
    capture_queue_t     q = {0};
    capture_counters_t  c = {0};
    for (int i = 0; i < n; ++i) {
        capture_gate_in_t in = {
            .gate_open      = open,
            .fps_applied    = fps,
            .stop_requested = false,
        };
        capture_gate_out_t out;
        memset(&out, 0, sizeof(out));
        capture_gated_iteration(&q, &c, &in, &out);
        TEST_ASSERT_EQUAL_UINT32(open ? (1000u / fps)
                                      : CAPTURE_IDLE_PERIOD_MS,
                                 out.period_ms);
        TEST_ASSERT_EQUAL_INT(open, out.ran);
    }
}

/* ---------- S:Registered — dispatch retires not_implemented --- */
TEST_CASE(
    "test_stream_registered_dispatches_stop_ack_no_not_implemented "
    "[fw-19][scenario-Registered]",
    "[stream-cmd][fw-19]")
{
    stream_reset();
    viewer_join();

    /* Viewerless STOP is the simplest registered path (D4): the
     * FW-18 state produced not_implemented; now the handler owns
     * it and answers the ruling-1 envelope byte-exactly. */
    dispatch("{\"cmd\":\"stream\",\"on\":false,\"id\":\"s\"}");

    char ack[128];
    ack_text_at(0, ack, sizeof(ack));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"stream_ok\",\"on\":false,\"fps\":5}", ack);
    TEST_ASSERT_FALSE(capture_running_get()); /* idempotent stop */
}

/* ---------- S:Default — absent fps echoes CONFIG default ------ */
TEST_CASE(
    "test_stream_start_absent_fps_echoes_config_default [fw-19.1]",
    "[stream-cmd][fw-19]")
{
    stream_reset();
    viewer_join();

    dispatch("{\"cmd\":\"stream\",\"on\":true}");

    char ack[128];
    ack_text_at(0, ack, sizeof(ack));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"stream_ok\",\"on\":true,\"fps\":5}", ack);
    TEST_ASSERT_TRUE(capture_running_get());
}

/* ---------- S:Echo — explicit fps echoed AS APPLIED ----------- */
TEST_CASE(
    "test_stream_start_explicit_fps_echoed_as_applied [fw-19.1]",
    "[stream-cmd][fw-19]")
{
    stream_reset();
    viewer_join();

    dispatch("{\"cmd\":\"stream\",\"on\":true,\"fps\":10}");

    char ack[128];
    ack_text_at(0, ack, sizeof(ack));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"stream_ok\",\"on\":true,\"fps\":10}", ack);
    TEST_ASSERT_TRUE(capture_running_get());
}

/* ---------- S:Clamp — silent range clamp (ruling 6) ----------- */
TEST_CASE(
    "test_stream_fps_range_clamps_silently_99_to_15_and_0_to_1 "
    "[fw-19.3][ruling-6]",
    "[stream-cmd][fw-19][clamp]")
{
    stream_reset();
    viewer_join();

    dispatch("{\"cmd\":\"stream\",\"on\":true,\"fps\":99}");
    dispatch("{\"cmd\":\"stream\",\"on\":true,\"fps\":0}");

    char ack[128];
    ack_text_at(0, ack, sizeof(ack));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"stream_ok\",\"on\":true,\"fps\":15}", ack);
    ack_text_at(1, ack, sizeof(ack));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"stream_ok\",\"on\":true,\"fps\":1}", ack);
    TEST_ASSERT_TRUE(capture_running_get());
}

/* ---------- S:BadField — wrong-typed fields, id lossless ------ */
TEST_CASE(
    "test_stream_bad_field_wrong_typed_on_or_fps_id_lossless "
    "[fw-19.4][ruling-6]",
    "[stream-cmd][fw-19]")
{
    stream_reset();
    viewer_join();

    /* fps as STRING / FLOAT / JSON-null, and REQUIRED on missing. */
    dispatch("{\"cmd\":\"stream\",\"on\":true,\"fps\":\"10\","
             "\"id\":\"e\"}");
    dispatch("{\"cmd\":\"stream\",\"on\":true,\"fps\":10.5,"
             "\"id\":42}");
    dispatch("{\"cmd\":\"stream\",\"on\":true,\"fps\":null}");
    dispatch("{\"cmd\":\"stream\",\"id\":\"q\"}");

    char env[128];
    ack_text_at(0, env, sizeof(env));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"bad_field\",\"id\":\"e\"}",
        env);
    ack_text_at(1, env, sizeof(env));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"bad_field\",\"id\":42}",
        env);
    ack_text_at(2, env, sizeof(env));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"bad_field\"}", env);
    ack_text_at(3, env, sizeof(env));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"bad_field\",\"id\":\"q\"}",
        env);

    /* Field rejection NEVER touches the gate (validate-first). */
    TEST_ASSERT_FALSE(capture_running_get());
}

/* ---------- S:NoViewer — pre-start state guard (ruling 7) ----- */
TEST_CASE(
    "test_stream_start_without_viewer_refused_gate_stays_stopped "
    "[fw-19.5][ruling-7]",
    "[stream-cmd][fw-19]")
{
    stream_reset();
    /* Recorder WITHOUT a handshake: the fd-probe guard still sees
     * no viewer while the sink can record the refusal. */
    TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_install());
    TEST_ASSERT_FALSE(ws_server_viewer_active());

    dispatch("{\"cmd\":\"stream\",\"on\":true,\"id\":\"n\"}");

    char env[128];
    ack_text_at(0, env, sizeof(env));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"no_viewer\",\"id\":\"n\"}",
        env);
    TEST_ASSERT_FALSE(capture_running_get()); /* gate never opens */

    /* ...and the NEXT sim-second acquires NOTHING (default-
     * stopped gate stepped at the idle cadence). */
    step_ticks(false, CONFIG_FIRMWARE_STREAM_FPS, 10);
    TEST_ASSERT_EQUAL_INT(0, mock_esp_camera_fb_get_call_count());
}

/* ---------- S:Ordering — design-D3 ack-before-frame pin ------- */
TEST_CASE(
    "test_stream_ack_precedes_frame_data_ring_order "
    "[fw-19.2][design-d3][ordering]",
    "[stream-cmd][fw-19]")
{
    stream_reset();
    /* Real server sink (NO recorder): only the httpd mock ring
     * preserves text-vs-binary ARRIVAL ORDER. */
    TEST_ASSERT_EQUAL(ESP_OK, softap_sta_listener_install());
    TEST_ASSERT_EQUAL(ESP_OK, ws_server_install());
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_esp_event_fire_handler(IP_EVENT,
                                    IP_EVENT_STA_GOT_IP,
                                    NULL));
    mock_httpd_req_t *req = mock_httpd_req_new();
    req->method = HTTP_GET;
    req->sockfd = 7;
    TEST_ASSERT_EQUAL(ESP_OK, mock_httpd_invoke_registered_handler(
                                  CONFIG_FIRMWARE_WS_PATH,
                                  HTTP_GET, req));
    mock_httpd_req_free(req);

    /* [0]=hello(text). Handler completes BEFORE returning, so the
     * ack is on the wire before any frame can be produced. */
    dispatch("{\"cmd\":\"stream\",\"on\":true,\"fps\":10}");
    TEST_ASSERT_EQUAL_INT(2, mock_httpd_ws_send_call_count());
    char f[96];
    int type = -1;
    size_t flen = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_ws_get_frame_at(1, &type, (uint8_t *)f,
                                   sizeof(f) - 1, &flen));
    f[flen] = '\0';
    TEST_ASSERT_EQUAL_INT(HTTPD_WS_TYPE_TEXT, type);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"stream_ok\",\"on\":true,\"fps\":10}", f);

    /* First gated step produces a frame; shipping it lands a
     * BINARY frame at [2] — nothing text interleaved ahead of it. */
    step_ticks(true, 10, 1);
    uint8_t frame_bytes[8] = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
        ws_sink_send_bin(frame_bytes, sizeof(frame_bytes)));
    uint8_t slot[8];
    size_t blen = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
        mock_httpd_ws_get_frame_at(2, &type, slot, sizeof(slot),
                                   &blen));
    TEST_ASSERT_EQUAL_INT(HTTPD_WS_TYPE_BINARY, type);
    TEST_ASSERT_EQUAL_INT(3, mock_httpd_ws_send_call_count());
}

/* ---------- S:StopHalt — halt ≤1 sim-s, single ack, quiet ----- */
TEST_CASE(
    "test_stream_stop_halts_within_one_simulated_second "
    "[fw-19.2][ruling-1][ruling-4]",
    "[stream-cmd][fw-19]")
{
    stream_reset();
    viewer_join();

    dispatch("{\"cmd\":\"stream\",\"on\":true,\"fps\":10}");
    step_ticks(true, 10, 3); /* three paced acquisitions */
    int during = mock_esp_camera_fb_get_call_count();
    TEST_ASSERT_EQUAL_INT(3, during);

    /* Explicit stop: clear-then-ack (D3 pin), echoing CURRENT
     * applied fps — NOT the CONFIG default. */
    dispatch("{\"cmd\":\"stream\",\"on\":false,\"id\":9}");
    char ack[128];
    ack_text_at(1, ack, sizeof(ack));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"stream_ok\",\"on\":false,\"fps\":10}", ack);
    TEST_ASSERT_FALSE(capture_running_get());

    /* EXACTLY ONE stop ack (idempotent latch — no duplicates). */
    TEST_ASSERT_EQUAL_INT(2, (int)ws_sink_recorder_text_count());

    /* One FULL simulated second at the closed-gate idle cadence:
     * zero acquisitions, zero further traffic. */
    int before = mock_esp_camera_fb_get_call_count();
    step_ticks(false, 10, 10);
    TEST_ASSERT_EQUAL_INT(before,
                          mock_esp_camera_fb_get_call_count());
    TEST_ASSERT_EQUAL_INT(2, (int)ws_sink_recorder_text_count());
}
