/* test_capture_ws_guard.c — FW-19.5 deep-defense bite-proof guard
 * (design D7/D8; folded into FW-19 U4 / Pass 15).
 *
 * This file compiles in TWO builds:
 *
 *   - PRODUCTION (Pass 1, no flags): pins the viewer-state linkage —
 *     with NO active WS viewer and no stream.on ever issued, the
 *     gate stays STOPPED and gated ticks never run an acquisition.
 *
 *   - STUB BUILD (Pass 15, -DCAPTURE_TEST_STUB_IGNORE_WS_STATE=1,
 *     applied to BOTH capture.c AND this file): arms the host-only
 *     tripwire INSIDE capture_gated_iteration and drives one open-
 *     gate tick while the real viewer state reads DISCONNECTED — a
 *     model of a build where the WS-state dependency was ignored.
 *     The tripwire MUST fire with the verbatim message "capture
 *     MUST NOT run while the WS viewer is disconnected." Pass 15
 *     greps for exactly that single expected failure.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "capture.h"
#include "ws_server.h"

#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

TEST_CASE(
    "test_guard_capture_never_runs_while_ws_viewer_disconnected "
    "[fw-19.5][guard][bite-proof]",
    "[capture][fw-19.5][guard]")
{
    /* Fresh device state: no viewer session, gate default-stopped. */
    ws_server_reset_for_test();
    capture_gate_reset_for_test();
    capture_counters_reset_for_test();

#ifdef CAPTURE_TEST_STUB_IGNORE_WS_STATE
    /* ---- Pass 15 stub build: the bite target is live. ---- */
    TEST_ASSERT_FALSE_MESSAGE(ws_server_viewer_active(),
                              "stub fixture broken: viewer unexpectedly "
                              "active");

    /* One OPEN-gate tick with NOTHING consulting the WS state —
     * exactly what a stubbed-out dependency would produce. With the
     * tripwire armed inside capture_gated_iteration, ran=true while
     * the viewer is disconnected fires TEST_FAIL_MESSAGE verbatim;
     * the belt-and-braces assertion below is unreachable then. */
    capture_queue_t q = {0};
    capture_counters_t c = {0};
    capture_gate_in_t in = { .gate_open   = true,
                             .fps_applied = CONFIG_FIRMWARE_STREAM_FPS,
                             .stop_requested = false };
    capture_gate_out_t out;
    capture_gated_iteration(&q, &c, &in, &out);
    /* NO test-side assertion here BY DESIGN: when the armed tripwire
     * inside capture_gated_iteration sees ran=true while the viewer
     * reads disconnected, ITS verbatim TEST_FAIL_MESSAGE is the one
     * expected failure. If that production tripwire were ever
     * removed, this test would silently PASS and Pass 15 would
     * fail-to-trip — exactly the regression signal wanted. */
#else
    /* ---- Production build: pin the viewer-state linkage. ---- */
    TEST_ASSERT_FALSE_MESSAGE(ws_server_viewer_active(),
                              "fixture broken: viewer unexpectedly "
                              "active");

    /* Wrapper semantics per tick: snapshot the gate (still STOPPED —
     * no stream.on could have arrived without a viewer), consume any
     * stop word, take one gated step. Five idle ticks, zero runs. */
    capture_queue_t q = {0};
    capture_counters_t c = {0};
    for (int tick = 0; tick < 5; ++tick) {
        capture_gate_in_t in = {
            .gate_open      = capture_running_get(),
            .fps_applied    = capture_fps_get(),
            .stop_requested = capture_stop_request_take(),
        };
        capture_gate_out_t out;
        capture_gated_iteration(&q, &c, &in, &out);
        TEST_ASSERT_FALSE_MESSAGE(
            out.ran,
            "viewer-state linkage lost: acquisition ran with no "
            "viewer and no stream start");
        TEST_ASSERT_FALSE(capture_running_get());
    }
    TEST_ASSERT_EQUAL_UINT32(0, c.frames_captured);
    TEST_ASSERT_EQUAL_UINT32(0, capture_frames_captured_get());
#endif
}
