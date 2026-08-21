/* test_boot_fail_loud.c — FW-03.2 bite-proof + green path
 *
 * Spec: "failing init is named in the error log." The bite-proof
 * asserts:
 *   1. `boot_status_t.step == BOOT_STEP_CAMERA_INIT` (typed error).
 *   2. The error log line contains the substring "camera" — via
 *      `boot_step_str(BOOT_STEP_CAMERA_INIT)` returning "camera_init".
 *      The orchestrator's ESP_LOGE emits `step=%s err=%s` with the
 *      result of `boot_step_str(step)`, so any orchestrator that
 *      forgets to include the step name in the log fails the bite-proof.
 *   3. `mock_supervision_count() == 0` — the orchestrator halted
 *      between camera_init and supervision, so no tasks fired.
 *
 * The green path test (also in this file) asserts that on the
 * happy path:
 *   1. `boot_status_t.ret == ESP_OK`.
 *   2. NO error log line is emitted (mock_log_error_count == 0).
 *   3. Exactly 4 supervision records (health, capture, stream, control).
 *
 * The host runner compiles THIS FILE for both the production build
 * (with all FW-03.2 tests active) and the stub build (with
 * `-DBOOT_TEST_STUB_FAIL_LOUD=0`, see tools/run_host_tests.py Pass 3
 * for FW-03.4 — Pass 2 here is the same compile as production for
 * fail-loud; no separate stub-flag is needed for FW-03.2 since the
 * orchestrator's fail-loud is exercised in BOTH builds).
 */
#include "mock_nvs_flash_link.h"
#include "mock_boot_link.h"

#include <string.h>

#include "boot.h"
#include "boot_status.h"
#include "mock_boot_button.h"
#include "mock_init_returns.h"
#include "mock_log.h"
#include "mock_supervision_record.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

static void seed_configured_nvs(void)
{
    mock_nvs_reset();
    mock_init_returns_reset();
    mock_log_reset();
    mock_supervision_reset();
    mock_boot_button_set(false);
    mock_nvs_seed_u8("config", "schema_version", CONFIG_SCHEMA_VERSION);
    mock_nvs_seed_str("config", "ssid", "home-2.4");
    mock_nvs_seed_str("config", "password", "secret");
}

TEST_CASE(
    "boot fails loud at camera_init when forced non-OK [fw-03.2][bite-proof]",
    "[boot][fw-03.2][bite-proof]")
{
    seed_configured_nvs();
    mock_init_returns_set(BOOT_STEP_CAMERA_INIT, ESP_FAIL);

    boot_status_t s = boot_run();

    /* Tagged error in code. */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_INT(BOOT_STEP_CAMERA_INIT, s.step);

    /* Error log line contains the failing step name. The
     * orchestrator emits `step=<boot_step_str(step)> err=<esp_err>`
     * — so `step=camera_init` is in the captured line, which
     * contains the substring "camera". */
    TEST_ASSERT_GREATER_THAN_size_t(0u, mock_log_error_count);
    TEST_ASSERT_NOT_NULL(strstr(mock_log_last_error, "camera"));

    /* Halt: no supervision tasks fired. */
    TEST_ASSERT_EQUAL_size_t(0u, mock_supervision_count());
}

TEST_CASE(
    "boot green path returns ESP_OK with no error log [fw-03.2][green]",
    "[boot][fw-03.2][green]")
{
    seed_configured_nvs();

    boot_status_t s = boot_run();

    TEST_ASSERT_EQUAL_INT(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_INT(BOOT_STEP_RETURN, s.step);
    TEST_ASSERT_EQUAL_size_t(0u, mock_log_error_count);
    TEST_ASSERT_EQUAL_size_t(4u, mock_supervision_count());
}