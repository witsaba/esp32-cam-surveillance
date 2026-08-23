/* test_camera_psram_size.c — FW-10.4 PSRAM size log assertion.
 *
 * The camera component MUST emit one
 *   ESP_LOGI("camera", "psram_size=<N> bytes", size)
 * line at first init, where N equals esp_psram_get_size().
 * The test primes the mock's PSRAM size to a deterministic
 * value (4194304 = 4 MB) and asserts the matching log
 * substring reaches mock_log_last_info.
 *
 * Conventions: mock_log_last_info captures the most-recent
 * ESP_LOGI message; the regex `psram_size=\d+ bytes` matches
 * "psram_size=4194304 bytes" by hand-written literal (a true
 * regex match is overkill for a 1-test leaf; substring find
 * keeps the host test runner dependency-free).
 *
 * Triangulation: the S1 (present allows init) test from
 * test_camera_psram.c also implicitly exercises this log line
 * when camera_init() is called with PSRAM present. The
 * dedicated test below adds a strict assertion on the
 * recorded INFO log so the contract is explicitly load-bearing.
 */
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_log.h"

#include "camera.h"
#include "config.h"
#include "esp_err.h"
#include "unity.h"

#include <string.h>

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

TEST_CASE(
    "test_fw10_4_psram_size_logged_at_first_init [fw-10.4][green]",
    "[camera][fw-10.4][psram-size]")
{
    mock_esp_camera_reset();
    mock_log_reset();

    /* Prime PSRAM with a deterministic 4 MB size — the
     * production FW-10.4 contract is "log the size in bytes",
     * the literal value is the mock's job. */
    mock_esp_camera_prime_psram(true, 4194304);
    mock_esp_camera_init_return_set(ESP_OK);

    config_t cfg = {0};
    esp_err_t rc = camera_init(&cfg);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* The log line MUST contain "psram_size=<digits> bytes".
     * We don't enforce the exact integer (the mock's printf
     * formatting owns that surface — %u is unsigned int) but
     * we assert the prefix + suffix, plus the integer string
     * substring for the primed 4194304 = 4 MB. */
    TEST_ASSERT_NOT_NULL(mock_log_last_info);
    /* Substring `psram_size=` MUST appear. */
    TEST_ASSERT_NOT_NULL(strstr(mock_log_last_info, "psram_size="));
    /* Substring `bytes` MUST appear. */
    TEST_ASSERT_NOT_NULL(strstr(mock_log_last_info, "bytes"));
    /* The sized integer — `4194304` (the primed value) — MUST
     * appear in the log line. */
    TEST_ASSERT_NOT_NULL(strstr(mock_log_last_info, "4194304"));
}
