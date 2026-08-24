/* test_soft_recovery_window.c — FW-16.1 sliding-window threshold core
 * (R-FW16-1.1 boundary + expiry scenarios).
 *
 * The window core is PURE (design AD3): every scenario drives explicit
 * microsecond timestamps through health_window_record() — no clock mock,
 * no IDF dependency, no FreeRTOS. The production clock (esp_timer_get_time)
 * enters only in components/health/health.c, which feeds this API.
 *
 * Scenarios under test (threshold = 30, window = 10 min = 600 s):
 *
 *   S1  29 in-window failures            → should_recover == false
 *   S2  30th failure lands in-window     → should_recover == true
 *   S3  31st in-window failure           → trigger condition holds
 *   S4  15 failures spread over 20 min   → stale entries pruned,
 *                                          should_recover == false
 *   S5  boundary: an entry exactly at
 *       now - window_us is kept; one
 *       µs past it is pruned              → lazy-prune edge pinned
 *
 * Trigger rule (AD3): fire iff in-window count >= threshold.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "health_window.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#define THRESHOLD      30u
#define WINDOW_MIN_US  ((int64_t)600 * 1000000) /* 10 minutes in µs */

/* ---------- S1 — 29 in-window failures do not trigger ---------- */
TEST_CASE(
    "test_soft_recovery_29_in_window_failures_do_not_trigger [fw-16.1][window][scenario-S1]",
    "[health][fw-16.1][window]")
{
    health_window_t w;
    health_window_reset(&w);

    for (int i = 0; i < 29; ++i) {
        /* One DISTINCT outage episode every 10 s — all 29 fit
         * inside the window (GOT_IP closes each episode). */
        (void)health_window_record(&w, (int64_t)i * 10 * 1000000,
                                   WINDOW_MIN_US);
        health_window_mark_reconnected(&w);
    }

    TEST_ASSERT_EQUAL_size_t(29, health_window_count(&w));
    TEST_ASSERT_FALSE(health_window_should_recover(&w, THRESHOLD));
}

/* ---------- S2 — the 30th in-window failure triggers ---------- */
TEST_CASE(
    "test_soft_recovery_30th_in_window_failure_triggers [fw-16.1][window][scenario-S2]",
    "[health][fw-16.1][window]")
{
    health_window_t w;
    health_window_reset(&w);

    for (int i = 0; i < 29; ++i) {
        (void)health_window_record(&w, (int64_t)i * 10 * 1000000,
                                   WINDOW_MIN_US);
        health_window_mark_reconnected(&w);
    }
    TEST_ASSERT_FALSE(health_window_should_recover(&w, THRESHOLD));

    /* The 30th lands at t = 290 s as a NEW episode — the whole
     * burst spans under five minutes, so nothing has been pruned
     * when it arrives. */
    size_t count = health_window_record(&w, (int64_t)29 * 10 * 1000000,
                                        WINDOW_MIN_US);

    TEST_ASSERT_EQUAL_size_t(30, count);
    TEST_ASSERT_TRUE(health_window_should_recover(&w, THRESHOLD));
}

/* ---------- S3 — a 31st in-window failure holds the trigger ---------- */
TEST_CASE(
    "test_soft_recovery_31_in_window_failures_hold_trigger [fw-16.1][window][scenario-S3]",
    "[health][fw-16.1][window]")
{
    health_window_t w;
    health_window_reset(&w);

    for (int i = 0; i < 31; ++i) {
        (void)health_window_record(&w, (int64_t)i * 10 * 1000000,
                                   WINDOW_MIN_US);
        health_window_mark_reconnected(&w);
    }

    TEST_ASSERT_EQUAL_size_t(31, health_window_count(&w));
    TEST_ASSERT_TRUE(health_window_should_recover(&w, THRESHOLD));
}

/* ---------- S4 — 15 failures over 20 min are pruned ---------- */
TEST_CASE(
    "test_soft_recovery_15_failures_over_20_minutes_pruned_no_trigger [fw-16.1][window][scenario-S4]",
    "[health][fw-16.1][window]")
{
    health_window_t w;
    health_window_reset(&w);

    /* One failure every 80 s (t = 0 .. 1040 s), with the 15th and
     * last landing AT the evaluation instant now = 1200 s. At that
     * instant only entries with ts > 1200 s − 600 s survive:
     * t ∈ {640, 720, 800, 880, 960, 1040, 1200} → exactly 7. */
    for (int i = 0; i < 14; ++i) {
        (void)health_window_record(&w, (int64_t)i * 80 * 1000000,
                                   WINDOW_MIN_US);
        health_window_mark_reconnected(&w);
    }
    int64_t now_us = (int64_t)1200 * 1000000;
    (void)health_window_record(&w, now_us, WINDOW_MIN_US);

    TEST_ASSERT_EQUAL_size_t(7, health_window_count(&w));
    TEST_ASSERT_FALSE(health_window_should_recover(&w, THRESHOLD));
}

/* ---------- S5 — lazy-prune window boundary ---------- */
TEST_CASE(
    "test_soft_recovery_window_boundary_entry_kept_then_expired [fw-16.1][window][boundary]",
    "[health][fw-16.1][window]")
{
    health_window_t w;
    health_window_reset(&w);

    /* A single failure at t = 0. Evaluated AT the window edge
     * (now == ts + window_us) it is still in-window per AD3
     * ("drops entries < now − window_us" — equality is kept). */
    (void)health_window_record(&w, 0, WINDOW_MIN_US);
    health_window_mark_reconnected(&w);
    (void)health_window_record(&w, WINDOW_MIN_US, WINDOW_MIN_US);
    TEST_ASSERT_EQUAL_size_t(2, health_window_count(&w));

    /* One µs past the edge the t=0 entry expires on the next
     * ingest. Without lazy pruning the count would grow to 3; the
     * expired entry + fresh append cancel out to exactly 2. */
    (void)health_window_record(&w, WINDOW_MIN_US + 1, WINDOW_MIN_US);
    TEST_ASSERT_EQUAL_size_t(2, health_window_count(&w));
}
