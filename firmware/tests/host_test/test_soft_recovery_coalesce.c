/* test_soft_recovery_coalesce.c — FW-16.1 episode coalescing
 * (R-FW16-1.1; design AD2).
 *
 * Coalescing rule = EPISODE LATCH, not a time gap:
 *
 *   - initial latch is CLOSED, so a drop observed before any
 *     GOT_IP counts as a real failure;
 *   - the FIRST DISCONNECTED with the latch closed appends ONE
 *     timestamp and OPENS the outage episode;
 *   - further DISCONNECTEDs while latched are reconnect retries
 *     inside the SAME outage (wifi backoff re-emits them) and do
 *     NOT increment;
 *   - GOT_IP closes the episode (health_window_mark_reconnected)
 *     so the next drop starts a NEW counted failure.
 *
 * This kills the FW-14 paired/double-count class structurally:
 * one physical fault advances the counter by EXACTLY ONE.
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

#define WINDOW_US ((int64_t)600 * 1000000)

/* ---------- C1 — paired-event burst advances counter by exactly 1 ---------- */
TEST_CASE(
    "test_soft_recovery_paired_burst_advances_counter_exactly_one [fw-16.1][coalesce][scenario-C1]",
    "[health][fw-16.1][coalesce]")
{
    health_window_t w;
    health_window_reset(&w);

    /* First DISCONNECTED of the outage: counts. */
    size_t n1 = health_window_record(&w, 1000000, WINDOW_US);
    TEST_ASSERT_EQUAL_size_t(1, n1);

    /* Paired retry burst (FW-14 double-count class): same outage,
     * latch open → NO additional increments. */
    size_t n2 = health_window_record(&w, 1100000, WINDOW_US);
    TEST_ASSERT_EQUAL_size_t(1, n2);
    size_t n3 = health_window_record(&w, 1200000, WINDOW_US);
    TEST_ASSERT_EQUAL_size_t(1, n3);

    TEST_ASSERT_EQUAL_size_t(1, health_window_count(&w));
}

/* ---------- C2 — GOT_IP closes the episode so the next drop counts ---------- */
TEST_CASE(
    "test_soft_recovery_got_ip_closes_episode_next_drop_counts [fw-16.1][coalesce][scenario-C2]",
    "[health][fw-16.1][coalesce]")
{
    health_window_t w;
    health_window_reset(&w);

    /* Outage #1: one counted failure despite retries. */
    (void)health_window_record(&w, 1000000, WINDOW_US);
    (void)health_window_record(&w, 1100000, WINDOW_US);
    TEST_ASSERT_EQUAL_size_t(1, health_window_count(&w));

    /* GOT_IP: the episode closes. */
    health_window_mark_reconnected(&w);

    /* Outage #2: a NEW physical fault → second counted failure. */
    size_t n = health_window_record(&w, 5000000, WINDOW_US);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_size_t(2, health_window_count(&w));
}

/* ---------- C3 — initial latch closed: pre-GOT_IP drop counts ---------- */
TEST_CASE(
    "test_soft_recovery_initial_latch_closed_first_drop_ever_counts [fw-16.1][coalesce][scenario-C3]",
    "[health][fw-16.1][coalesce]")
{
    health_window_t w;
    health_window_reset(&w);

    /* A device that boots straight into an outage (no IP ever
     * observed yet) must still count its first drop — AD2 pins
     * the initial latch state to CLOSED. */
    size_t n = health_window_record(&w, 42, WINDOW_US);
    TEST_ASSERT_EQUAL_size_t(1, n);
}

/* ---------- C4 — coalesced episodes still drive the threshold ---------- */
TEST_CASE(
    "test_soft_recovery_distinct_episodes_accumulate_to_threshold [fw-16.1][coalesce][scenario-C4]",
    "[health][fw-16.1][coalesce]")
{
    const uint32_t threshold = 30u;
    health_window_t w;
    health_window_reset(&w);

    /* 29 distinct outages (each with a noisy retry burst), each
     * closed by GOT_IP before the next drop. Exactly 29 counted
     * failures — one below the trigger. */
    int64_t t = 1000000;
    for (uint32_t i = 0; i < 29; ++i) {
        (void)health_window_record(&w, t, WINDOW_US);        /* drop   */
        (void)health_window_record(&w, t + 10000, WINDOW_US);/* retry burst */
        (void)health_window_record(&w, t + 20000, WINDOW_US);/* retry burst */
        health_window_mark_reconnected(&w);                  /* GOT_IP */
        t += 20 * 1000000;                                   /* +20 s  */
    }
    TEST_ASSERT_EQUAL_size_t(29, health_window_count(&w));
    TEST_ASSERT_FALSE(health_window_should_recover(&w, threshold));

    /* Episode #30 crosses the threshold. */
    (void)health_window_record(&w, t, WINDOW_US);
    TEST_ASSERT_EQUAL_size_t(30, health_window_count(&w));
    TEST_ASSERT_TRUE(health_window_should_recover(&w, threshold));
}
