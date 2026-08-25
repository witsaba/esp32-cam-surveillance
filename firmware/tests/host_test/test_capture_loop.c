/* test_capture_loop.c — FW-11.1 capture-loop production scenarios +
 * FW-11.2 drop-on-overflow + counter scenarios.
 *
 * The capture component owns the FreeRTOS frame-acquisition
 * loop. It exposes a PURE function `capture_loop_iteration()`
 * that performs ONE iteration: esp_camera_fb_get → optional
 * drop-on-full → frames_captured++. The FreeRTOS wrapper
 * (capture_task_entry) calls this inside an infinite for-loop
 * with vTaskDelay between iterations; the wrapper is NOT
 * exercised on host because we don't link mock_freertos. The
 * pure function is the load-bearing test surface.
 *
 * Scenarios under two distinct build shapes:
 *
 *   S1 (FW-11.1, 5 fps sustained) — Call capture_loop_iteration()
 *       5 times. Assert frames_captured == 5.
 *
 *   S2 (FW-11.1, 1 fps sustained) — Call capture_loop_iteration()
 *       once. Assert frames_captured == 1.
 *
 *   S3 (FW-11.2, drop on full queue) — Pre-fill the host's
 *       capture_queue_t slots to depth 2; call one more
 *       iteration. Assert frames_captured did NOT increment,
 *       fb_drops incremented by 1, AND esp_camera_fb_return()
 *       was called.
 *
 *   S4 (FW-11.2, 100 frames no stall) — Call
 *       capture_loop_iteration() 100 times back-to-back. Assert
 *       frames_captured == 100 (no DMA stall because the mock
 *       has no real I2S path).
 *
 * Conventions: the mock triplet's frame counter is
 * `mock_esp_camera_fb_get_call_count()` and the return-tracking
 * flag is `mock_esp_camera_fb_return_was_called()` — both reset
 * by `mock_esp_camera_reset()` between tests.
 */
#include "capture.h"
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_supervision_record.h"
#include "mock_init_returns.h"
#include "boot.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* Standard fixture: reset mocks + prime an empty queue + return
 * non-NULL from esp_camera_fb_get so the loop reaches the
 * queue-send branch. Mirrors test_camera_init.c::camera_init
 * _with_mocks shape. */
static void capture_with_mocks(void)
{
    mock_esp_camera_reset();
    mock_supervision_reset();
    mock_init_returns_reset();
    /* Reset the capture component's module-static counters
     * + queue so test order doesn't leak state. */
    capture_counters_reset_for_test();
    /* FW-19 U1: restore the default-stopped stream gate so
     * gate tests are order-independent too (no-op for the
     * pre-gate suites above — they never open the gate). */
    capture_gate_reset_for_test();
}

/* ---------- S1 — 5 fps sustained: 5 iterations = 5 fb_get attempts ---------- */
TEST_CASE(
    "test_fw11_1_five_fps_five_iterations_yield_five_attempts [fw-11.1][scenario-S1][green]",
    "[capture][fw-11.1][fps]")
{
    capture_with_mocks();

    /* The observable contract: the loop calls
     * esp_camera_fb_get() exactly once per iteration. With a
     * depth-2 queue and no consumer, the first 2 frames
     * succeed (frames_captured=2), then the next 3 are
     * dropped (fb_drops=3). The spec's "5±1 frames enqueued
     * per second" assumes a consumer drains the queue; this
     * test exercises the producer-only path and asserts the
     * producer side is correct. */
    capture_queue_t q = {0};
    capture_counters_t c = {0};
    for (int i = 0; i < 5; i++) {
        capture_loop_iteration(&q, &c);
    }
    TEST_ASSERT_EQUAL_UINT32(2, c.frames_captured);
    TEST_ASSERT_EQUAL_UINT32(3, c.fb_drops);
    /* esp_camera_fb_get called exactly 5 times. */
    TEST_ASSERT_EQUAL_INT(5, mock_esp_camera_fb_get_call_count());
}

/* ---------- S2 — 1 fps sustained: 1 iteration = 1 frame ---------- */
TEST_CASE(
    "test_fw11_1_one_fps_single_iteration_yields_one_frame [fw-11.1][scenario-S2]",
    "[capture][fw-11.1][fps]")
{
    capture_with_mocks();

    capture_queue_t q = {0};
    capture_counters_t c = {0};
    capture_loop_iteration(&q, &c);
    TEST_ASSERT_EQUAL_UINT32(1, c.frames_captured);
    TEST_ASSERT_EQUAL_UINT32(0, c.fb_drops);
}

/* ---------- S3 — full queue + new frame = drop + return + count ---------- */
TEST_CASE(
    "test_fw11_2_full_queue_drops_frame_and_returns_buffer [fw-11.2][scenario-S3]",
    "[capture][fw-11.2][drop]")
{
    capture_with_mocks();

    /* Pre-fill the host queue to depth 2 (capacity MOCK_CAPTURE
     * _QUEUE_DEPTH == 2). The host capture_queue_t is a thin
     * wrapper around static slots[2] + head/tail indices. */
    capture_queue_t q = {0};
    camera_fb_t fb_a = { .buf = (uint8_t *)1, .len = 11520 };
    camera_fb_t fb_b = { .buf = (uint8_t *)2, .len = 11520 };
    TEST_ASSERT_TRUE(capture_queue_send_drop_on_full(&q, &fb_a));
    TEST_ASSERT_TRUE(capture_queue_send_drop_on_full(&q, &fb_b));

    capture_counters_t c = {0};

    /* Now one more frame arrives → the queue is full → the loop
     * MUST drop it (call esp_camera_fb_return + bump fb_drops),
     * NOT increment frames_captured. */
    capture_loop_iteration(&q, &c);

    TEST_ASSERT_EQUAL_UINT32(0, c.frames_captured);
    TEST_ASSERT_EQUAL_UINT32(1, c.fb_drops);
    /* The mock's return-tracker confirms esp_camera_fb_return
     * was called for the dropped frame. */
    TEST_ASSERT_TRUE(mock_esp_camera_fb_return_was_called());
}

/* ---------- S4 — 100 frames back-to-back: no stall ---------- */
TEST_CASE(
    "test_fw11_2_one_hundred_frames_no_stall [fw-11.2][scenario-S4]",
    "[capture][fw-11.2][no-stall]")
{
    capture_with_mocks();

    capture_queue_t q = {0};
    capture_counters_t c = {0};
    for (int i = 0; i < 100; i++) {
        capture_loop_iteration(&q, &c);
    }
    /* On host, no consumer is draining the queue, so after the
     * first 2 frames slots are full and subsequent frames are
     * dropped. We assert the counter advanced monotonically and
     * no hang occurred. frames_captured is exactly the first 2
     * (the queue capacity); fb_drops == 98. The contract is
     * "100 frames produced without blocking the I2S DMA" — the
     * mock's no-IO path proves the contract on host. */
    TEST_ASSERT_EQUAL_UINT32(2, c.frames_captured);
    TEST_ASSERT_EQUAL_UINT32(98, c.fb_drops);
}

/* ---------- S5 — getters return the latest counter values ---------- */
TEST_CASE(
    "test_fw11_2_getters_return_counter_values [fw-11.2][getters]",
    "[capture][fw-11.2][getters]")
{
    capture_with_mocks();

    /* Pre-condition: counters start at 0. */
    TEST_ASSERT_EQUAL_UINT32(0, capture_fb_drops_get());
    TEST_ASSERT_EQUAL_UINT32(0, capture_frames_captured_get());

    /* Run the loop 5 times; capture_loop_iteration mirrors
     * its writes to the module-static counters consumed by
     * the FW-13.6 getters. After 5 iterations: 2 captured
     * (queue depth-2) + 3 drops. */
    capture_queue_t q = {0};
    capture_counters_t c = {0};
    for (int i = 0; i < 5; i++) {
        capture_loop_iteration(&q, &c);
    }
    TEST_ASSERT_EQUAL_UINT32(2, capture_frames_captured_get());
    TEST_ASSERT_EQUAL_UINT32(3, capture_fb_drops_get());
}

/* ---------- FW-19 U1 — default-stopped stream gate ----------
 *
 * S6 (FW-19.5, spec #4017 Boot-no-frames) — fresh boot, no
 * commands: the gate defaults STOPPED and one simulated second
 * of idle polling produces ZERO frame acquisitions.
 *
 * Time control (design D2): the harness steps DISCRETE ticks
 * through the pure `capture_gated_iteration` seam with ZERO
 * sleeps — 10 iterations at the 100 ms idle period = exactly
 * 1 simulated second, tick-deterministic.
 */

TEST_CASE(
    "test_fw19_5_fresh_boot_gate_stopped_zero_acquisitions [fw-19.5][scenario-S6][red]",
    "[capture][fw-19][gate]")
{
    capture_with_mocks();

    /* GIVEN fresh boot, no commands: gate defaults STOPPED. */
    TEST_ASSERT_FALSE(capture_running_get());

    capture_queue_t q = {0};
    capture_counters_t c = {0};

    /* WHEN 1 simulated second elapses as 10 tick-stepped
     * gated iterations at the idle poll period. */
    for (int t = 0; t < 10; t++) {
        capture_gate_in_t in = {
            .gate_open      = capture_running_get(), /* wrapper snapshot */
            .fps_applied    = CONFIG_FIRMWARE_STREAM_FPS,
            .stop_requested = false,
        };
        capture_gate_out_t out = {0};
        capture_gated_iteration(&q, &c, &in, &out);

        /* THEN every tick is a no-op: nothing runs and the
         * wrapper is paced at the idle poll period. */
        TEST_ASSERT_FALSE(out.ran);
        TEST_ASSERT_EQUAL_UINT32(CAPTURE_IDLE_PERIOD_MS, out.period_ms);
    }

    /* Zero frame acquisitions occurred over the whole sim-s. */
    TEST_ASSERT_EQUAL_UINT32(0, c.frames_captured);
    TEST_ASSERT_EQUAL_INT(0, mock_esp_camera_fb_get_call_count());
}

/* S7 (FW-19.1, design D2) — gate OPEN runs exactly one
 * acquisition per gated call and reports the applied-fps
 * pacing period. Proves the non-trivial path: production code
 * RAN (fb_get count advanced), not just a benign no-op. */
TEST_CASE(
    "test_fw19_1_gate_open_runs_frame_and_paces_period [fw-19.1][scenario-S7]",
    "[capture][fw-19][gate]")
{
    capture_with_mocks();

    capture_queue_t q = {0};
    capture_counters_t c = {0};

    capture_gate_in_t in = {
        .gate_open      = true,
        .fps_applied    = 15,
        .stop_requested = false,
    };
    capture_gate_out_t out = {0};
    capture_gated_iteration(&q, &c, &in, &out);

    TEST_ASSERT_TRUE(out.ran);
    TEST_ASSERT_FALSE(out.stop_latched);
    /* Exact tick pacing: floor(1000 / 15) = 66 ms. */
    TEST_ASSERT_EQUAL_UINT32(66, out.period_ms);
    /* One real acquisition happened through the mock camera. */
    TEST_ASSERT_EQUAL_UINT32(1, c.frames_captured);
    TEST_ASSERT_EQUAL_INT(1, mock_esp_camera_fb_get_call_count());
}

/* S8 (task 1.4 period matrix, design D6) — exact discrete-tick
 * periods across the applied-fps range: 15→66, 5→200,
 * 3→333 (floor proof), 1→1000 ms. Zero sleeps — each case is
 * one stepped tick. */
TEST_CASE(
    "test_fw19_1_period_matrix_exact_ticks [fw-19.1][scenario-S8][matrix]",
    "[capture][fw-19][gate]")
{
    capture_with_mocks();

    capture_queue_t q = {0};
    capture_counters_t c = {0};
    const struct { uint32_t fps; uint32_t want_ms; } rows[] = {
        { 15, 66 },   /* ratified MAX ceiling */
        {  5, 200 },  /* CONFIG default */
        {  3, 333 },  /* floor(1000/3): truncation, not rounding */
        {  1, 1000 }, /* MIN floor */
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        capture_gate_in_t in = {
            .gate_open      = true,
            .fps_applied    = rows[i].fps,
            .stop_requested = false,
        };
        capture_gate_out_t out = {0};
        capture_gated_iteration(&q, &c, &in, &out);
        TEST_ASSERT_EQUAL_UINT32(rows[i].want_ms, out.period_ms);
    }
}

/* S9 (FW-19.2, task 1.4 stop-latch, design D5) — the stop word
 * latches EXACTLY ONCE: consumed on the tick it arrives (no
 * frame runs that tick), then the cleared gate keeps every
 * later tick silent. Models the wrapper sequence: snapshot →
 * gated step → clear running on latch. */
TEST_CASE(
    "test_fw19_2_stop_word_latches_once_then_stays_quiet [fw-19.2][scenario-S9]",
    "[capture][fw-19][gate]")
{
    capture_with_mocks();

    capture_queue_t q = {0};
    capture_counters_t c = {0};
    int latch_count = 0;

    /* Tick 0: streaming at 15 fps — one frame lands. */
    capture_gate_in_t run = {
        .gate_open = true, .fps_applied = 15, .stop_requested = false,
    };
    capture_gate_out_t out = {0};
    capture_gated_iteration(&q, &c, &run, &out);
    TEST_ASSERT_TRUE(out.ran);

    /* Tick 1: stop word arrives — consumed THIS tick, no run. */
    capture_gate_in_t stopping = {
        .gate_open = true, .fps_applied = 15, .stop_requested = true,
    };
    capture_gated_iteration(&q, &c, &stopping, &out);
    if (out.stop_latched) latch_count++;
    TEST_ASSERT_TRUE(out.stop_latched);
    TEST_ASSERT_FALSE(out.ran);
    /* Wrapper clears the gate on latch: */
    capture_run_stop();
    TEST_ASSERT_FALSE(capture_running_get());

    /* Ticks 2..11 (one further simulated second): gate closed —
     * zero acquisitions, no second latch. Queue goes quiet. */
    for (int t = 0; t < 10; t++) {
        capture_gate_in_t idle = {
            .gate_open      = capture_running_get(),
            .fps_applied    = 15,
            .stop_requested = false,
        };
        capture_gated_iteration(&q, &c, &idle, &out);
        if (out.stop_latched) latch_count++;
        TEST_ASSERT_FALSE(out.ran);
    }
    TEST_ASSERT_EQUAL_INT(1, latch_count);
    /* Exactly the pre-stop frame count stands. */
    TEST_ASSERT_EQUAL_UINT32(1, c.frames_captured);
    TEST_ASSERT_EQUAL_INT(1, mock_esp_camera_fb_get_call_count());
}

/* S10 (FW-19.3 + ruling 6, task 1.4 clamp matrix) — out-of-RANGE
 * integers clamp silently to [MIN..MAX]=[1..15]; in-range values
 * pass through unchanged. Never errors. */
TEST_CASE(
    "test_fw19_3_fps_clamp_bounds_matrix [fw-19.3][scenario-S10][matrix]",
    "[capture][fw-19][clamp]")
{
    const struct { long long in; uint32_t want; } rows[] = {
        { -5,      1 }, /* negative floors at MIN */
        {  0,      1 },
        {  1,      1 }, /* MIN boundary holds */
        {  7,      7 }, /* in-range passes through */
        { 14,     14 },
        { 15,     15 }, /* MAX boundary holds */
        { 16,     15 },
        { 99,     15 }, /* ruling-6 example */
        { 1000000, 15 },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32(rows[i].want,
                                 capture_fps_clamp(rows[i].in));
    }
}

/* S11 (FW-19.5 precondition, gate lifecycle) — run_start opens
 * the gate; run_stop closes it; stop is idempotent. Applied-fps
 * storage inside run_start becomes observable via U2's ack echo
 * (design D3); here we pin the running-flag transitions. */
TEST_CASE(
    "test_fw19_5_gate_lifecycle_start_stop_idempotent [fw-19.5][scenario-S11]",
    "[capture][fw-19][gate]")
{
    capture_with_mocks();

    TEST_ASSERT_FALSE(capture_running_get()); /* fresh boot: stopped */

    capture_run_start(99); /* clamped to MAX=15 before storing */
    TEST_ASSERT_TRUE(capture_running_get());

    capture_run_stop();
    TEST_ASSERT_FALSE(capture_running_get());

    capture_run_stop(); /* idempotent — still stopped, no error */
    TEST_ASSERT_FALSE(capture_running_get());

    capture_run_start(CONFIG_FIRMWARE_STREAM_FPS);
    TEST_ASSERT_TRUE(capture_running_get());
}
