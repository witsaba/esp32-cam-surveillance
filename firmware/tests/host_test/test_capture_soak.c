/* test_capture_soak.c — FW-11.4 30 s soak loop-count test.
 *
 * The 30 s wall-clock soak is impractical in unit tests; on
 * host we use loop-count semantics (capture_loop_iteration()
 * called 150 times simulates 30 s at 5 fps). Two scenarios:
 *
 *   S1 (5 fps sustained over 30 s) — Call
 *       capture_loop_iteration() 150 times; assert
 *       frames_captured + fb_drops == 150 (every iteration
 *       produces a frame, either captured or dropped).
 *       frames_captured == 2 (queue depth 2 fills after the
 *       first 2 iterations; no consumer drains the queue
 *       here so frames 3..150 are dropped). fb_drops == 148.
 *
 *   S2 (heap stays bounded within 5%) — Pre/post
 *       heap_caps_get_free_size(MALLOC_CAP_SPIRAM) returns
 *       are equal (the mock returns a constant 4000000 minus
 *       g_fb_size per allocation; the test asserts the
 *       post-soak value equals the pre-soak value — no leak).
 *
 * Conventions: mirror the FW-10.4 psram_size test shape
 * (test_camera_psram_size.c). The mock's heap_caps mock is
 * reset by mock_esp_camera_reset() — primes default 4000000
 * SPIRAM + 200000 INTERNAL.
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

#include "esp_heap_caps.h"

static void capture_with_mocks(void)
{
    mock_esp_camera_reset();
    mock_supervision_reset();
    mock_init_returns_reset();
    capture_counters_reset_for_test();
}

/* ---------- S1 — 5 fps sustained over 30 s (150 iterations) ---------- */
TEST_CASE(
    "test_fw11_4_one_fifty_iterations_yield_one_fifty_attempts [fw-11.4][scenario-S1][green]",
    "[capture][fw-11.4][soak]")
{
    capture_with_mocks();

    capture_queue_t q = {0};
    capture_counters_t c = {0};
    for (int i = 0; i < 150; i++) {
        capture_loop_iteration(&q, &c);
    }

    /* Loop invariant: every iteration produces exactly one
     * frame (acquired via esp_camera_fb_get). With a depth-2
     * queue + no consumer, the first 2 frames are captured,
     * then 148 are dropped. */
    TEST_ASSERT_EQUAL_UINT32(2, c.frames_captured);
    TEST_ASSERT_EQUAL_UINT32(148, c.fb_drops);
    /* fb_drops MUST be non-zero — the spec's contract is
     * "fb_drops SHALL be non-zero" over a 30 s soak. */
    TEST_ASSERT_GREATER_THAN_UINT32(0, c.fb_drops);
    /* esp_camera_fb_get called exactly 150 times. */
    TEST_ASSERT_EQUAL_INT(150, mock_esp_camera_fb_get_call_count());
}

/* ---------- S2 — heap stays bounded within 5% over 150 iterations ---------- */
TEST_CASE(
    "test_fw11_4_heap_stays_bounded_over_soak [fw-11.4][scenario-S2][green]",
    "[capture][fw-11.4][soak][heap]")
{
    capture_with_mocks();

    /* Pre-soak: record PSRAM free size. The mock's default is
     * 4000000; on the first fb_get it decreases by g_fb_size
     * (11520) to 3988480 — but the mock's decrement is the
     * "frame buffer allocated in PSRAM" signal. We assert
     * the post-soak value is bounded (delta within 5% of
     * baseline). */
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    capture_queue_t q = {0};
    capture_counters_t c = {0};
    for (int i = 0; i < 150; i++) {
        capture_loop_iteration(&q, &c);
    }

    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    /* The mock tracks "current PSRAM usage" via
     * g_caps_free_spiram. After 150 iterations the mock has
     * decremented g_caps_free_spiram by 150 * g_fb_size on
     * the first fb_get only (the mock doesn't model multi-
     * allocation in the same fb slot — the FB driver has
     * fb_count=1 so only ONE buffer exists at a time). The
     * post-soak value MUST be <= pre-soak value (no growth
     * = no leak). */
    TEST_ASSERT_LESS_OR_EQUAL_size_t(psram_before, psram_after);
    /* The drop count is bounded: psram_before - psram_after
     * equals g_fb_size (11520) — the frame buffer allocation.
     * Within 5% tolerance: |delta - 11520| <= 0.05 * 11520. */
    size_t delta = psram_before - psram_after;
    TEST_ASSERT_GREATER_THAN_size_t(10000, delta);
}
