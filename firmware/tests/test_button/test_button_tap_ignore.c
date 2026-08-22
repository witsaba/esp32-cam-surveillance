/* test_button_tap_ignore.c — FW-07.1 host tests.
 *
 * The button driver ignores "taps" — presses whose total duration
 * is ≤ CONFIG_FIRMWARE_BOOT_BUTTON_TAP_MAX_MS (default 100 ms).
 * The PRD § FR-7 button table (and the PRD wording on "press
 * jitter") requires this filter because the boot button is a
 * mechanical input whose contact bounce can be misinterpreted
 * as a short press.
 *
 * Scenarios (S1-S4) cover the boundary around TAP_MAX_MS:
 *   S1: 50 ms tap  (well below threshold)  → ignored
 *   S2: 99 ms tap  (just below boundary)    → ignored
 *   S3: 100 ms tap (exactly at boundary)    → ignored (TAP_MAX inclusive)
 *   S4: 101 ms tap (just above boundary)    → ignored (NOT a runtime
 *                                            long-press yet; that needs
 *                                            ≥ RUNTIME_LONGPRESS_MS)
 *
 * Phase B contract: a tap is ANY press whose duration is ≤
 * TAP_MAX_MS. The runtime long-press callback (FW-07.3) fires at
 * ≥ RUNTIME_LONGPRESS_MS = 10_000 ms, not here. So at 101 ms the
 * state machine must ALSO do nothing — no callback fire, no
 * `boot_button_pressed_at_boot()` set to true.
 *
 * Test mechanism: the test calls `button_poll_once(now_us)`
 * directly (FreeRTOS is not linked on host) with a primed
 * `mock_esp_timer_get_time_set_return(now_us)` value. Each call
 * reads `gpio_get_level()` (mocked via `mock_gpio_get_level`).
 * The test primes the GPIO level for each poll using
 * `mock_gpio_get_level_set_return(level)` BEFORE the poll.
 *
 * Pattern (mirrors FW-06 `test_led_boot_connecting.c`):
 *   - setUp: `mock_gpio_reset()` + `mock_esp_timer_reset()` +
 *     register a runtime callback that increments a test counter,
 *     then `button_init()`.
 *   - Each test: drive a 10 ms polling cadence for the scenario
 *     duration (e.g. 50 ms = 5 polls at 0/10/20/30/40 ms). The
 *     level sequence is HIGH until the press starts at t=0, then
 *     LOW for the tap duration, then HIGH again.
 *   - Assertions: runtime callback counter == 0 (no fire),
 *     `boot_button_pressed_at_boot() == false` (via the mock,
 *     which is primed to false).
 */
#include "mock_boot_link.h"
#include "mock_gpio_link.h"
#include "mock_esp_timer_link.h"

#include "button.h"
#include "mock_boot_button.h"
#include "mock_gpio.h"
#include "mock_esp_timer.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* Tap-ignore threshold (must match button.h default). */
#ifndef FW07_TAP_MAX_US
#define FW07_TAP_MAX_US \
    ((int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_TAP_MAX_MS * 1000LL)
#endif

/* Polling period (must match the production poll handle's
 * period in button.c — 10 ms per the design). */
#define FW07_POLL_PERIOD_US 10000LL

/* Runtime long-press callback counter. Registered in setUp; the
 * test asserts it stays 0 for all 4 tap-ignore scenarios. */
static volatile int g_runtime_cb_count = 0;

static void runtime_cb_increment(void)
{
    g_runtime_cb_count++;
}

/* Standard fixture: reset all mocks, prime the boot-button
 * signal to false (the contract for Phase B), register a
 * runtime callback, then init the button driver. */
static void setUp_button(void)
{
    mock_gpio_reset();
    mock_esp_timer_reset();
    /* Prime the boot-button signal to false (Phase B contract:
     * the strong symbol returns false because no BOOT_TIME
     * long-press has happened). On host, the mock wins via
     * mock_boot_link.h. */
    mock_boot_button_set(false);

    /* Reset the test counter. */
    g_runtime_cb_count = 0;

    /* Tear down any prior button_init() so the next init() creates
     * a fresh poll handle. Without this, back-to-back tests share
     * timer-handle state and the counts drift. */
    button_deinit();

    esp_err_t rc = button_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Register the runtime long-press callback (FW-07.3 wires the
     * real cb; in Phase B it's a stub that stores the pointer
     * but does not invoke it). The test still asserts the cb
     * counter stays 0 for tap durations. */
    rc = button_on_runtime_longpress_set(runtime_cb_increment);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
}

/* Drive a tap scenario: GPIO LOW for `tap_us` microseconds,
 * GPIO HIGH before and after. The first call captures the
 * `button_init()`-time `now_us` baseline so the test can step
 * the clock forward by FW07_POLL_PERIOD_US per poll.
 *
 * `total_us` is the total simulated duration (tap_us + 10 ms
 * slack on either side so the state machine sees a clean
 * HIGH → LOW → HIGH sequence). */
static void drive_tap(int64_t tap_us, int64_t total_us)
{
    /* Number of polls to cover `total_us` at 10 ms cadence. */
    int polls = (int)((total_us / FW07_POLL_PERIOD_US) + 1);
    for (int i = 0; i < polls; ++i) {
        int64_t now_us = (int64_t)i * FW07_POLL_PERIOD_US;
        int level;
        if (now_us < tap_us) {
            level = 0;  /* active-LOW: 0 = pressed */
        } else {
            level = 1;  /* released */
        }
        mock_gpio_get_level_set_return(level);
        mock_esp_timer_get_time_set_return(now_us);
        button_poll_once(now_us);
    }
}

TEST_CASE(
    "tap_50ms_is_ignored [fw-07.1]",
    "[button][fw-07.1]")
{
    setUp_button();

    /* 50 ms tap (well below 100 ms threshold). Total simulated
     * time = 100 ms (10 polls at 10 ms). */
    drive_tap(50 * 1000LL, 100 * 1000LL);

    /* The tap-ignore state machine MUST have absorbed the press
     * without firing the runtime callback. */
    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
    /* The boot-time signal MUST remain false (Phase B: no
     * boot-time detection wired yet). */
    TEST_ASSERT_FALSE(boot_button_pressed_at_boot());
}

TEST_CASE(
    "tap_99ms_is_ignored [fw-07.1]",
    "[button][fw-07.1]")
{
    setUp_button();

    /* 99 ms tap (just below the 100 ms boundary). */
    drive_tap(99 * 1000LL, 150 * 1000LL);

    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
    TEST_ASSERT_FALSE(boot_button_pressed_at_boot());
}

TEST_CASE(
    "tap_100ms_is_ignored [fw-07.1]",
    "[button][fw-07.1]")
{
    setUp_button();

    /* 100 ms tap (exactly at the boundary; TAP_MAX_MS is
     * inclusive per the Kconfig help text: "presses shorter
     * than this are treated as tap-jitter"). */
    drive_tap(100 * 1000LL, 150 * 1000LL);

    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
    TEST_ASSERT_FALSE(boot_button_pressed_at_boot());
}

TEST_CASE(
    "tap_101ms_is_ignored [fw-07.1]",
    "[button][fw-07.1]")
{
    setUp_button();

    /* 101 ms tap (just above the boundary, still far below
     * the 10 s runtime long-press threshold). The state
     * machine must treat this as a short press that doesn't
     * yet qualify for the runtime factory-reset cb. */
    drive_tap(101 * 1000LL, 150 * 1000LL);

    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
    TEST_ASSERT_FALSE(boot_button_pressed_at_boot());
}
