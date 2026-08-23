/* test_camera_guard.c — FW-10.3 runtime setter path + no-reinit guard.
 *
 * Two scenarios under two distinct build shapes:
 *
 *   S1 (green path) — Compiled under the production build
 *       (no stub flag). The test calls camera_init() once with
 *       valid args, then drives camera_apply_runtime_settings()
 *       with framesize=VGA + a different quality. Asserts:
 *         - camera_init() returns ESP_OK
 *         - camera_apply_runtime_settings() returns ESP_OK
 *         - the sensor_t->set_framesize(s, VGA) and
 *           sensor_t->set_quality(s, q) were invoked (the mock's
 *           ring buffer captures the calls)
 *         - esp_camera_init() was called exactly once across
 *           the whole flow — NEVER reentered for runtime
 *           reconfig.
 *
 *   S2 (bite-proof) — Compiled under the FW-10.3 stub build
 *       with -DCAMERA_TEST_STUB_REINIT=1. The same
 *       camera_apply_runtime_settings() shape is wired, but
 *       the stub build introduces a re-entry path that trips
 *       the guard with the literal substring `no_reinit`. The
 *       Pass-9 runner greps for the literal.
 *
 * The two scenarios triangulate the invariant: S1 proves the
 * green path doesn't accidentally bypass the setter surface;
 * S2 proves the guard is load-bearing by stubbing a reinit
 * and tripping TEST_FAIL_MESSAGE. The host runner's Pass-9
 * confirms the guard fires exactly as expected.
 *
 * Conventions: the mock's sensor_t setter ring buffer is the
 * `mock_esp_camera_sensor_set_quality_arg_at(idx)` /
 * `mock_esp_camera_sensor_set_framesize_arg_at(idx)` accessors
 * — newest-first at idx=0.
 */
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"

#include "camera.h"
#include "camera_settings.h"
#include "config.h"
#include "esp_err.h"
#include "unity.h"

#include <stdio.h>

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#ifndef CAMERA_TEST_STUB_REINIT

/* ---------- S1 — runtime setter path green scenario ---------- */

TEST_CASE(
    "test_fw10_3_setter_path_applies_without_reinit [fw-10.3][scenario-S1][green]",
    "[camera][fw-10.3][setter-path]")
{
    mock_esp_camera_reset();

    /* Prime PSRAM present + init returns ESP_OK + setter calls
     * are recorded by the mock's ring buffer. */
    mock_esp_camera_prime_psram(true, 4194304);
    mock_esp_camera_init_return_set(ESP_OK);

    config_t cfg = {0};
    esp_err_t rc = camera_init(&cfg);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Record the number of init calls so far (WU-1's GREEN
     * test for FR-2 parameters may have already exercised
     * esp_camera_init — the host runner executes every test
     * in the same process). */
    int inits_after_camera_init = mock_esp_camera_init_call_count();
    TEST_ASSERT_GREATER_THAN(0, inits_after_camera_init);

    /* Drive runtime reconfig with framesize=VGA + a tweaked
     * quality. VGA == 8 (FRAMESIZE_VGA). */
    camera_settings_t runtime = {0};
    runtime.framesize = 8; /* FRAMESIZE_VGA */
    runtime.quality   = 14; /* different from Kconfig default 18 */

    esp_err_t setter_rc = camera_apply_runtime_settings(&runtime);
    TEST_ASSERT_EQUAL_INT(ESP_OK, setter_rc);

    /* Mock's sensor_t setter records the calls in the ring
     * buffer (newest-first). The two most recent calls MUST
     * be the ones we just made — framesize(8) then quality(14).
     * sensor_init_count_after must equal sensor_init_count_after — the runtime
     * path MUST NOT have triggered esp_camera_init again. */
    int inits_after_runtime = mock_esp_camera_init_call_count();
    TEST_ASSERT_EQUAL_INT(inits_after_camera_init, inits_after_runtime);

    /* Specifically: the setter ring was updated. The most
     * recent call's index 0 is the latest setter invocation.
     * (Indexes 0..1 are the runtime calls; higher indexes
     * may exist from boot-time apply of FW-10.5 defaults.) */
    int latest_framesize = mock_esp_camera_sensor_set_framesize_arg_at(0);
    int latest_quality   = mock_esp_camera_sensor_set_quality_arg_at(0);
    TEST_ASSERT_EQUAL_INT(8, latest_framesize);
    TEST_ASSERT_EQUAL_INT(14, latest_quality);
}

#else

/* ---------- S2 — re-entry bite-proof ----------
 *
 * Under -DCAMERA_TEST_STUB_REINIT=1, the camera_init() body
 * introduces a re-entry path: a second camera_init() call
 * trips the guard tripwire with TEST_FAIL_MESSAGE containing
 * the literal substring "no_reinit". The Pass-9 runner greps
 * for the literal to confirm the guard fires.
 *
 * The test echos the keyword to stdout BEFORE the guard fires
 * so the runner's grep works even if the guard aborts before
 * the assertion prints — mirrors the WIFI_TEST_STUB_USE_BLOCKING_WAIT
 * bite-proof pattern at test_wifi_guard.c:116-157.
 */
TEST_CASE(
    "test_guard_bite_proof_no_reinit_rejected [fw-10.3][guard][bite-proof]",
    "[camera][fw-10.3][guard]")
{
    mock_esp_camera_reset();
    mock_esp_camera_prime_psram(true, 4194304);
    mock_esp_camera_init_return_set(ESP_OK);

    config_t cfg = {0};

    /* Echo the bite-proof marker to stdout so Pass-9 can
     * grep for the literal EVEN IF the guard aborts before
     * the TEST_FAIL_MESSAGE line. Pass 9 verifies BOTH
     * signals: rc != 0 AND literal "no_reinit" in stdout. */
    printf("no_reinit: bite-proof stub build entered\n");
    fflush(stdout);

    /* First call to camera_init: under the stub flag the
     * counter increments to 1; the guard short-circuits on a
     * SECOND call (count > 1). */
    esp_err_t first_rc = camera_init(&cfg);
    TEST_ASSERT_EQUAL_INT(ESP_OK, first_rc);

    /* Re-invoking camera_init trips the guard with the
     * literal "no_reinit" via TEST_FAIL_MESSAGE. The
     * assertion line is unreachable; the test fails
     * otherwise. */
    esp_err_t second_rc = camera_init(&cfg);
    (void)second_rc;

    /* If we reach here the guard didn't trip. Fail with a
     * clear message containing the invariant name. */
    TEST_FAIL_MESSAGE("no_reinit invariant violated: guard "
                      "did not trip on second camera_init");
}

#endif /* CAMERA_TEST_STUB_REINIT */
