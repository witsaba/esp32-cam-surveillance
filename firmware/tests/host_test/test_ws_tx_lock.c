/* test_ws_tx_lock.c — viewer-sink TX serialization guard.
 *
 * Root cause (live-wire proven; predates FW-18): stream (BIN),
 * control replies and the 30 s status timer write the SAME viewer
 * fd through the ws.c dispatch seam unsynchronized → interleaved
 * wire frames. Invariant: each ws_sink_send_* call is an ATOMIC
 * decision-and-send unit.
 *   S1 overlap: rendezvous-released threads hammer the seam; the
 *      recorder depth detector must report ZERO simultaneous sends.
 *   S2 accounting: lifetime total == threads × iterations (ring
 *      eviction defeats per-slot counting at this volume).
 *   S3 integrity: retained slots hold byte-exact payloads.
 * HONESTY: unfixed, failure is probabilistic — N=4000/thread makes
 * simultaneous entry near-certain. PORTABILITY: macOS lacks
 * pthread_barrier_t (verified); atomic gate = same rendezvous. */
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>

#include "ws.h"
#include "ws_sink_recorder.h"

#define TX_THREADS 2
#define TX_ITERS   4000
#define TX_BIN_LEN 64

/* Start gate: neither worker sends until BOTH are parked here. */
static atomic_int s_gate;

static void *tx_worker(void *arg)
{
    const char tag = (arg == NULL) ? 'A' : 'B';
    char    text[32];
    uint8_t bin[TX_BIN_LEN];

    atomic_fetch_add(&s_gate, 1);
    while (atomic_load_explicit(&s_gate, memory_order_acquire)
           < TX_THREADS) {
        sched_yield();
    }

    /* Fixed alternation keeps expected per-type totals exact. */
    for (int i = 0; i < TX_ITERS; ++i) {
        if (i % 2 == 0) {
            int n = snprintf(text, sizeof(text), "TX%c:%d", tag, i);
            (void)ws_sink_send_text(text, (size_t)n);
        } else {
            memset(bin, (uint8_t)tag, sizeof(bin));
            bin[0] = (uint8_t)tag;
            (void)ws_sink_send_bin(bin, sizeof(bin));
        }
    }
    return NULL;
}

TEST_CASE("test_ws_tx_lock_no_concurrent_sink_dispatch [fw-tx-lock]",
          "[ws][tx-lock]")
{
    TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_install());
    ws_sink_recorder_reset();
    atomic_store(&s_gate, 0);

    pthread_t th[TX_THREADS];
    for (int i = 0; i < TX_THREADS; ++i) {
        TEST_ASSERT_EQUAL_MESSAGE(
            0, pthread_create(&th[i], NULL, tx_worker,
                              i == 0 ? NULL : (void *)1),
            "worker spawn");
    }
    for (int i = 0; i < TX_THREADS; ++i) {
        TEST_ASSERT_EQUAL(0, pthread_join(th[i], NULL));
    }

    /* S1 — THE invariant: zero overlapping sink dispatches. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0, ws_sink_recorder_overlap_violations(),
        "TX overlap violation: two tasks were inside the viewer "
        "sink dispatch simultaneously — sends are not serialized");

    /* S2 — nothing lost, nothing duplicated (2*N total). */
    TEST_ASSERT_EQUAL_UINT32(TX_THREADS * TX_ITERS,
                             ws_sink_recorder_frames_total());

    /* S3 — retained slots byte-exact single-sender payloads. */
    char out[32];
    for (size_t i = 0; i < ws_sink_recorder_text_count(); ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_get_text_at(
                                      i, out, sizeof(out)));
        bool ok = (strlen(out) >= 5 && out[0] == 'T' && out[1] == 'X'
                   && (out[2] == 'A' || out[2] == 'B') && out[3] == ':');
        for (size_t j = 4; ok && j < strlen(out); ++j)
            ok = (out[j] >= '0' && out[j] <= '9');
        TEST_ASSERT_TRUE_MESSAGE(ok, "torn TEXT frame in recorder");
    }
    uint8_t bout[TX_BIN_LEN];
    size_t  blen = 0;
    for (size_t i = 0; i < ws_sink_recorder_bin_count(); ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, ws_sink_recorder_get_bin_at(
                                      i, bout, sizeof(bout), &blen));
        bool ok = (blen == TX_BIN_LEN
                   && (bout[0] == 'A' || bout[0] == 'B'));
        for (size_t j = 1; ok && j < blen; ++j)
            ok = (bout[j] == bout[0]);
        TEST_ASSERT_TRUE_MESSAGE(ok, "torn BIN frame in recorder");
    }
}
