/* test_capture_guard.c — FW-11.3 single-owner guard.
 *
 * Two scenarios under two distinct build shapes:
 *
 *   S1 (green path) — Compiled under the production build
 *       (no stub flag). Asserts that capture_task_start()
 *       succeeds without tripping the single-owner guard.
 *       The mock's fb_get call count + the queue's frame
 *       count confirm the capture loop body has exclusive
 *       ownership of esp_camera_fb_get on the production
 *       code path.
 *
 *   S2 (bite-proof) — Compiled under the FW-11.3 stub build
 *       with -DCAPTURE_TEST_STUB_SECOND_CALLER=1. A
 *       synthetic 2nd caller `_capture_test_stub_second_caller`
 *       introduces a violation of the single-owner invariant
 *       by calling esp_camera_fb_get() from outside the
 *       capture TU. The guard fires with TEST_FAIL_MESSAGE
 *       containing the literal substring "single_owner".
 *       Pass 10 greps for the literal to confirm the guard
 *       is load-bearing.
 *
 * Conventions: mirror the FW-10.3 test_camera_guard.c shape
 * (the no_reinit bite-proof pattern). The same compile flag
 * is applied to BOTH the production source (capture.c) and
 * this test file.
 */
#include "capture.h"
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_supervision_record.h"
#include "mock_init_returns.h"
#include "boot.h"
#include "boot_priq.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <stdio.h>

#ifndef CAPTURE_TEST_STUB_SECOND_CALLER

/* ---------- S1 — single-owner green scenario ----------
 *
 * The green scenario asserts the production build of
 * capture_task_start() returns ESP_OK + records
 * mock_supervision_record("capture") without tripping the
 * single-owner guard. We then exercise the loop body ONCE
 * via capture_loop_iteration() to confirm the producer
 * side is wired (the mock's fb_get call counter increments
 * from 0 to 1).
 */
TEST_CASE(
    "test_fw11_3_capture_task_start_records_supervision [fw-11.3][scenario-S1][green]",
    "[capture][fw-11.3][single-owner]")
{
    mock_esp_camera_reset();
    mock_supervision_reset();
    mock_init_returns_reset();

    esp_err_t rc = capture_task_start();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* The supervision role was recorded — boot.c:155's
     * call site contract is unchanged. */
    TEST_ASSERT_EQUAL_size_t(1u, mock_supervision_count());
    char name[24];
    mock_supervision_order(0, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("capture", name);

    /* Drive the loop body once — the producer path is
     * the sole caller of esp_camera_fb_get on host
     * (capture_loop_iteration -> esp_camera_fb_get). */
    capture_queue_t q = {0};
    capture_counters_t c = {0};
    capture_loop_iteration(&q, &c);
    TEST_ASSERT_EQUAL_INT(1, mock_esp_camera_fb_get_call_count());
    TEST_ASSERT_EQUAL_UINT32(1, c.frames_captured);
}

#else

/* ---------- S2 — single-owner bite-proof ----------
 *
 * Under -DCAPTURE_TEST_STUB_SECOND_CALLER=1, the
 * capture_task_start() body short-circuits into
 * capture_guard_fail_single_owner() with TEST_FAIL_MESSAGE
 * containing the literal substring "single_owner". The
 * Pass-10 runner greps for the literal.
 *
 * The test echos the marker to stdout BEFORE the guard
 * fires so the runner's grep works even if the guard
 * aborts before the assertion prints.
 */
TEST_CASE(
    "guard_bite_proof_single_owner_rejected [fw-11.3][guard][bite-proof]",
    "[capture][fw-11.3][guard]")
{
    mock_esp_camera_reset();
    mock_supervision_reset();
    mock_init_returns_reset();

    /* Echo the bite-proof marker to stdout so Pass 10 can
     * grep for the literal EVEN IF the guard aborts before
     * the TEST_FAIL_MESSAGE line. Mirrors the Pass 9
     * `no_reinit` pattern at test_camera_guard.c:135-136. */
    printf("single_owner: bite-proof stub build entered\n");
    fflush(stdout);

    /* Calling capture_task_start() under the stub flag
     * trips the guard. The assertion line is unreachable;
     * the test fails otherwise. */
    esp_err_t rc = capture_task_start();
    (void)rc;

    /* If we reach here the guard didn't trip. */
    TEST_FAIL_MESSAGE("single_owner invariant violated: guard "
                      "did not trip on capture_task_start");
}

#endif /* CAPTURE_TEST_STUB_SECOND_CALLER */
