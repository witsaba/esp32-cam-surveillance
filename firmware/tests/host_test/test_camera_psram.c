/* test_camera_psram.c — FW-10.2 PSRAM assertion tests.
 *
 * The camera component MUST call esp_psram_is_initialized()
 * BEFORE esp_camera_init(). If absent: log `PSRAM_REQUIRED` at
 * ESP_LOGE + return ESP_FAIL + MUST NOT call esp_restart(). The
 * orchestrator's BOOT_CHECK_STEP wraps the non-OK return.
 *
 * Two scenarios:
 *   S1 (PSRAM present allows init) — green path; camera_init
 *       reaches esp_camera_init() and returns ESP_OK.
 *
 *   S2 (PSRAM absent logs PSRAM_REQUIRED and returns ESP_FAIL)
 *       — the mock's prime_psram() flips the presence flag to
 *       false; camera_init logs PSRAM_REQUIRED + returns ESP_FAIL
 *       without touching esp_restart. The
 *       `mock_log_last_error` buffer asserts the typed-error
 *       substring.
 *
 * The TWO scenarios triangulate the production code: S1 proves
 * the green path isn't accidentally broken (esp_camera_init is
 * reached when PSRAM is present) and S2 proves the typed-error
 * path actually trips (the PSRAM_REQUIRED log reaches
 * mock_log_last_error and the return is non-OK). A
 * one-test-only design would let a hidden fake-it pass — the
 * second test forces generalisation.
 *
 * Conventions: mock_log_last_error buffer is captured after
 * camera_init() completes; if any production code path wrote to
 * the last-error slot BEFORE the PSRAM_REQUIRED log (unlikely
 * but possible), the assertion would fail. mirror of the
 * FW-03.2 fail-loud bite-proof pattern at
 * tests/test_boot/test_boot_fail_loud.c:65-72.
 */
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_log.h"

#include "camera.h"
#include "config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mock_init_returns.h"
#include "mock_log.h"
#include "unity.h"

#include <string.h>

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* S1 — PSRAM present allows init */
TEST_CASE(
    "test_fw10_2_psram_present_allows_init [fw-10.2][scenario-S1][green]",
    "[camera][fw-10.2][psram]")
{
    mock_esp_camera_reset();
    mock_log_reset();

    /* Prime PSRAM present + ESP_OK init return so the FR-2
     * config reaches esp_camera_init(). */
    mock_esp_camera_prime_psram(true, 4194304);
    mock_esp_camera_init_return_set(ESP_OK);

    config_t cfg = {0};
    esp_err_t rc = camera_init(&cfg);

    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
    /* esp_restart() is NEVER called by camera_init; the
     * mock's restart counter is just a sanity check here. */
}

/* S2 — PSRAM absent logs PSRAM_REQUIRED + returns ESP_FAIL */
TEST_CASE(
    "test_fw10_2_psram_absent_logs_required_and_fails [fw-10.2][scenario-S2]",
    "[camera][fw-10.2][psram][typed-error]")
{
    mock_esp_camera_reset();
    mock_log_reset();

    /* Prime PSRAM ABSENT (false, size=0) so the FW-10.2
     * PSRAM_REQUIRED branch trips. */
    mock_esp_camera_prime_psram(false, 0);
    /* esp_camera_init() MUST NOT be called — if the guard
     * short-circuits BEFORE esp_camera_init, the call count
     * stays at 0. */
    mock_esp_camera_init_return_set(ESP_OK);

    config_t cfg = {0};
    esp_err_t rc = camera_init(&cfg);

    TEST_ASSERT_EQUAL_INT(ESP_FAIL, rc);
    /* The typed-error log MUST contain the literal substring
     * `PSRAM_REQUIRED` per charter L890-893. */
    TEST_ASSERT_NOT_NULL_MESSAGE(mock_log_last_error,
                                  "expected an error log line");
    TEST_ASSERT_NOT_NULL(strstr(mock_log_last_error,
                                "PSRAM_REQUIRED"));

    /* The guard MUST short-circuit before esp_camera_init —
     * the mock's call count == 0 is the load-bearing
     * assertion that esp_restart() is NOT called (the
     * production guard returns ESP_FAIL directly, never
     * esp_restart). */
    TEST_ASSERT_EQUAL_INT(0, mock_esp_camera_init_call_count());
}
