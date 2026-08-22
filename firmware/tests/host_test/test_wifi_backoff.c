/* test_wifi_backoff.c — FW-08.1 6-row backoff schedule tests.
 *
 * The wifi component exposes a pure helper
 *   uint32_t wifi_backoff_delay_ms(uint32_t consecutive_failures)
 * that returns the delay for the Nth retry per the charter
 * L742-748 table:
 *
 *   | consecutive_failures | delay_ms |
 *   | 1 | 2000 |
 *   | 2 | 4000 |
 *   | 3 | 8000 |
 *   | 4 | 16000 |
 *   | 5 | 30000 |  <- cap reached
 *   | 6 | 30000 |  <- cap holds
 *
 * The 6 tests cover each row independently. They exercise the
 * PUBLIC helper directly — no IDF mocks are needed because the
 * helper is pure. The wifi.c stub body in T-08-A already
 * implements the table; the tests confirm it matches the
 * charter.
 *
 * The helper is the load-bearing piece for the FW-08 retry path;
 * the on_sta_disconnected handler (T-08-C) consumes its return
 * value to drive esp_timer_start_once.
 *
 * Convention: assertions use uint32_t cast because Unity on the
 * host build disables 64-bit support; the production type matches
 * the IDF esp_timer period argument (uint64_t) but the value
 * comfortably fits in 32 bits (max 30_000 ms = 30_000_000 us).
 */
#include "wifi.h"

#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

TEST_CASE(
    "test_fw08_1_backoff_failures_1 [fw-08.1][row-1]",
    "[wifi][fw-08.1][backoff]")
{
    TEST_ASSERT_EQUAL_UINT32(2000u, wifi_backoff_delay_ms(1));
}

TEST_CASE(
    "test_fw08_1_backoff_failures_2 [fw-08.1][row-2]",
    "[wifi][fw-08.1][backoff]")
{
    TEST_ASSERT_EQUAL_UINT32(4000u, wifi_backoff_delay_ms(2));
}

TEST_CASE(
    "test_fw08_1_backoff_failures_3 [fw-08.1][row-3]",
    "[wifi][fw-08.1][backoff]")
{
    TEST_ASSERT_EQUAL_UINT32(8000u, wifi_backoff_delay_ms(3));
}

TEST_CASE(
    "test_fw08_1_backoff_failures_4 [fw-08.1][row-4]",
    "[wifi][fw-08.1][backoff]")
{
    TEST_ASSERT_EQUAL_UINT32(16000u, wifi_backoff_delay_ms(4));
}

TEST_CASE(
    "test_fw08_1_backoff_failures_5 [fw-08.1][row-5][cap-reached]",
    "[wifi][fw-08.1][backoff]")
{
    TEST_ASSERT_EQUAL_UINT32(30000u, wifi_backoff_delay_ms(5));
}

TEST_CASE(
    "test_fw08_1_backoff_failures_6 [fw-08.1][row-6][cap-holds]",
    "[wifi][fw-08.1][backoff]")
{
    TEST_ASSERT_EQUAL_UINT32(30000u, wifi_backoff_delay_ms(6));
}
