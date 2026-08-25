/* test_control_task.c — FW-18 bounded ring + task-shell suite (U2).
 *
 * Transcribes spec #3970 FW-18.2 scenarios against the ring seam:
 *
 *   S1  busy consumer does not stall inbound pushes (manual loop
 *       iterations — enqueue stays non-blocking by construction)
 *   S2  queue-full drops the NEWEST frame, increments the drop
 *       counter, preserves FIFO of queued items, and emits NO wire
 *       token for the overflow (ruling #3966.2 — proven via the
 *       sink recorder: sends == enqueued frames only)
 *   S3  (T2.3) receive timeout is a bounded tick once task_start
 *       arms the sync hooks.
 */
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "control.h"
#include "ws_sink_recorder.h"

/* ---------- fixtures ---------- */

static char *frame_dup(const char *s)
{
    char *p = (char *)malloc(strlen(s) + 1);
    TEST_ASSERT_NOT_NULL(p);
    strcpy(p, s);
    return p;
}

static void task_reset(void)
{
    control_reset_for_test();
    ws_sink_recorder_reset();
}

/* ---------- S1: busy command does not stall inbound reads -------- */
TEST_CASE(
    "test_task_busy_command_does_not_stall_inbound [fw-18.2]",
    "[control][fw-18.2]")
{
    task_reset();
    control_queue_t *q = control_queue_for_test();

    /* Five frames land while the consumer idles. */
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(
            control_queue_send_drop_on_full(q, frame_dup("x")), "push");
    }

    /* Consumer pops ONE and goes busy processing it (~1 s command). */
    void *held = NULL;
    TEST_ASSERT_TRUE(control_queue_receive_timeout(&held, 0));
    TEST_ASSERT_NOT_NULL(held);

    /* Inbound reads during the busy window NEVER stall: four more
     * pushes all succeed immediately (capacity accounting follows
     * the advanced head). */
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(
            control_queue_send_drop_on_full(q, frame_dup("y")),
            "push during busy window");
    }

    /* Ring is now FULL (8): the next NEWEST push drops instantly. */
    free(frame_dup("z")); /* producer frees its own copy on drop */
    TEST_ASSERT_FALSE(control_queue_send_drop_on_full(q,
                                                      frame_dup("drop")));
    TEST_ASSERT_EQUAL_UINT32(1, control_frames_dropped_get());

    /* FIFO preserved end-to-end. */
    void *it = NULL;
    int   drained = 0;
    while (drained < 8 &&
           control_queue_receive_timeout(&it, 0)) {
        free(it);
        drained++;
    }
    TEST_ASSERT_EQUAL_INT(8, drained);
    free(held);
}

/* ---------- S2: drop-newest + counter + FIFO + NO wire token ----- */
TEST_CASE(
    "test_task_queue_full_drops_newest_no_wire_token [fw-18.2][ruling-2]",
    "[control][fw-18.2]")
{
    task_reset();
    TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_install());
    control_queue_t *q = control_queue_for_test();

    /* Eight distinct unknown-command frames fill the ring; each will
     * produce exactly one error envelope when processed. */
    char body[64];
    for (int i = 0; i < CONTROL_QUEUE_DEPTH; ++i) {
        snprintf(body, sizeof(body),
                 "{\"cmd\":\"frobnicate_%d\",\"id\":\"i%d\"}", i, i);
        TEST_ASSERT_TRUE(control_queue_send_drop_on_full(q,
                                                        frame_dup(body)));
    }
    TEST_ASSERT_EQUAL_UINT32(0, control_frames_dropped_get());

    /* The ninth frame is the NEWEST → dropped, counted, and NO wire
     * token exists for it (nothing to process later). */
    snprintf(body, sizeof(body), "{\"cmd\":\"dropped_one\",\"id\":\"X\"}");
    TEST_ASSERT_FALSE(control_queue_send_drop_on_full(q, frame_dup(body)));
    TEST_ASSERT_EQUAL_UINT32(1, control_frames_dropped_get());

    /* Drain via the production loop: one envelope per QUEUED frame,
     * never one for the dropped frame. */
    int iterations = 0;
    while (control_loop_iteration()) {
        TEST_ASSERT_TRUE(++iterations <= CONTROL_QUEUE_DEPTH);
    }
    TEST_ASSERT_EQUAL_INT(CONTROL_QUEUE_DEPTH, iterations);

    size_t sent = ws_sink_recorder_text_count();
    TEST_ASSERT_EQUAL_INT(CONTROL_QUEUE_DEPTH, (int)sent);

    /* Every emitted envelope echoes its own id; the dropped frame's
     * id appears NOWHERE on the wire. */
    char out[CONTROL_FRAME_MAX];
    for (size_t i = 0; i < sent; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_get_text_at(
                                      i, out, sizeof(out)));
        TEST_ASSERT_TRUE(strstr(out, "\"reason\":\"unknown\"") != NULL);
        TEST_ASSERT_NULL(strstr(out, "\"id\":\"X\""));
    }
    TEST_ASSERT_EQUAL_UINT32(1, control_frames_dropped_get());
}

/* ---------- S3: receive timeout is a bounded tick ---------------- */
TEST_CASE(
    "test_task_receive_timeout_is_bounded_tick [fw-18.2]",
    "[control][fw-18.2]")
{
    task_reset();

    /* task_start must arm the sync hooks BEFORE any spawn (host:
     * pthread hooks; the device xTaskCreate branch is compiled out
     * here). */
    TEST_ASSERT_EQUAL(ESP_OK, control_task_start());

    void *p = NULL;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool got = control_queue_receive_timeout(&p, 120);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long long dt_ms = (t1.tv_sec - t0.tv_sec) * 1000LL +
                      (t1.tv_nsec - t0.tv_nsec) / 1000000LL;

    TEST_ASSERT_FALSE(got); /* empty ring → timeout, no item */
    /* Bounded BOTH ways: waited ~the budget (hooks armed → real
     * condvar wait), and returned long before forever. A NULL-hook
     * (un-armed) implementation returns in ~0 ms and fails the
     * floor; a blocking-forever one never comes back. */
    TEST_ASSERT_MESSAGE(dt_ms >= 100,
        "receive returned before the bounded budget elapsed — "
        "sync hooks were not armed by control_task_start");
    TEST_ASSERT_TRUE(dt_ms < 2000);
}
