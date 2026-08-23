/* test_capture_heap.c — FW-11.5 PSRAM heap-metrics closing check.
 *
 * The capture component's first-frame `esp_camera_fb_get()`
 * MUST allocate the frame buffer in PSRAM
 * (MALLOC_CAP_SPIRAM), not internal SRAM. Verified by the
 * mock triplet's heap_caps_get_free_size(cap) returning
 * `psram_before - g_fb_size` after the first fb_get().
 *
 * Scenario:
 *   S1 (PSRAM heap decreases by frame-buffer allocation)
 *       — call capture_loop_iteration() once with the mock's
 *       primed g_fb_size = 11520 (QVGA JPEG per FW-10
 *       device-verify). Assert heap_caps_get_free_size(
 *       MALLOC_CAP_SPIRAM) decreased by exactly 11520 from
 *       the baseline. Also asserts the closing-check log
 *       reaches mock_log_last_info with the literal
 *       "psram_before=" substring.
 */
#include "capture.h"
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_log.h"
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

#include <string.h>

static void capture_with_mocks(void)
{
    mock_esp_camera_reset();
    mock_log_reset();
    mock_supervision_reset();
    mock_init_returns_reset();
    capture_counters_reset_for_test();
}

/* ---------- S1 — PSRAM heap decreases by frame-buffer allocation ---------- */
TEST_CASE(
    "test_fw11_5_psram_heap_decreases_by_frame_buffer_allocation [fw-11.5][scenario-S1][green]",
    "[capture][fw-11.5][psram]")
{
    capture_with_mocks();

    /* Pre-condition: mock primes PSRAM = 4000000. */
    size_t psram_baseline = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    TEST_ASSERT_EQUAL_size_t(4000000, psram_baseline);

    /* Run one iteration — the producer path calls
     * esp_camera_fb_get() once; the mock decrements PSRAM
     * by g_fb_size (11520 = the FW-10 device-verify frame-
     * buffer allocation). */
    capture_queue_t q = {0};
    capture_counters_t c = {0};
    capture_loop_iteration(&q, &c);

    size_t psram_after_first = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t delta = psram_baseline - psram_after_first;

    /* Delta MUST equal g_fb_size (11520) — the frame buffer
     * was allocated in PSRAM, not internal SRAM. */
    TEST_ASSERT_EQUAL_size_t(11520, delta);

    /* The closing-check log line MUST be emitted with the
     * literal "psram_before=" prefix so device-verified
     * builds can grep for it. */
    TEST_ASSERT_NOT_NULL(mock_log_last_info);
    TEST_ASSERT_NOT_NULL(strstr(mock_log_last_info, "psram_before="));
    TEST_ASSERT_NOT_NULL(strstr(mock_log_last_info, "psram_after="));

    /* frames_captured == 1 (first frame succeeded). */
    TEST_ASSERT_EQUAL_UINT32(1, c.frames_captured);
    TEST_ASSERT_EQUAL_UINT32(0, c.fb_drops);
}
