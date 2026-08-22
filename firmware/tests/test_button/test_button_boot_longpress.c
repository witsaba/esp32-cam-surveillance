/* test_button_boot_longpress.c — FW-07.2 host tests.
 *
 * The button driver asserts `boot_button_pressed_at_boot() == true`
 * iff the user held the button continuously for ≥
 * CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_LONGPRESS_MS (default 3000 ms)
 * during the BOOT_TIME phase. The latch is sticky — once asserted
 * during BOOT_TIME it stays asserted until `button_deinit()` or a
 * reboot.
 *
 * Phase C (FW-07.2) scenarios S5-S9 cover the boundary around
 * BOOT_LONGPRESS_MS + the strap-grace transient + the boot-time
 * window end:
 *   S5: 3 s boot-time press          → asserted
 *   S6: 10 s boot-time press         → asserted (NOT the runtime cb;
 *                                       runtime cb is gated on
 *                                       BUTTON_PHASE_RUNTIME which
 *                                       does not trigger during
 *                                       BOOT_TIME)
 *   S7: 2 s boot-time press (<3 s)   → NOT asserted
 *   S8: 500 ms strap-pin transient   → NOT asserted (absorbed by
 *                                       STRAP_GRACE window; no
 *                                       BOOT_TIME edge in flight)
 *   S9: 300 ms press during strap    → NOT asserted (entirely inside
 *                                       STRAP_GRACE window; release
 *                                       happens before grace ends)
 *
 * Test mechanism: same as Phase B (test_button_tap_ignore.c).
 * The test calls `button_poll_once(now_us)` directly with a
 * primed `mock_esp_timer_get_time()` value. Each call reads
 * `gpio_get_level()` (mocked via `mock_gpio_get_level`).
 *
 * IMPORTANT — calling the strong symbol on host:
 *
 * The `mock_boot_link.h` header `#define`s every reference to
 * `boot_button_pressed_at_boot()` to the mock implementation
 * `mock_boot_button_pressed_at_boot_impl()`. That redirect is
 * critical for the FW-03 determinism tests (which prime the return
 * value via `mock_boot_button_set(bool)`), but it would defeat
 * the FW-07.2 contract: the test must exercise the STRONG symbol
 * in `button.c` (the one that actually reads
 * `g_boot_button_pressed_at_boot`), not the mock.
 *
 * Pattern: include `mock_boot_link.h` for the GPIO + esp_timer
 * mocks, then `#undef boot_button_pressed_at_boot` so the test's
 * own assertions resolve to the strong symbol. The button driver
 * itself never includes `mock_boot_link.h` (it does not need to
 * redirect to itself), so production wiring is unaffected by
 * this `#undef`.
 */
#include "mock_boot_link.h"
#include "mock_gpio_link.h"
#include "mock_esp_timer_link.h"

/* Undefine the macro-redirect so `boot_button_pressed_at_boot()`
 * in this TU resolves to the strong symbol in button.c. See the
 * file header comment for the rationale. */
#undef boot_button_pressed_at_boot

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

/* Boot-time long-press threshold (must match button.h default). */
#ifndef FW07_BOOT_LONGPRESS_US
#define FW07_BOOT_LONGPRESS_US \
    ((int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_LONGPRESS_MS * 1000LL)
#endif

/* Boot-time window (default 5000 ms per Phase C design). The
 * state machine is in BOOT_TIME from STRAP_GRACE end to
 * BOOT_TIME end. */
#ifndef FW07_BOOT_TIME_WINDOW_US
#define FW07_BOOT_TIME_WINDOW_US \
    ((int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_TIME_WINDOW_MS * 1000LL)
#endif

/* Strap-grace window (must match button.h default). */
#ifndef FW07_STRAP_GRACE_US
#define FW07_STRAP_GRACE_US \
    ((int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_STRAP_GRACE_MS * 1000LL)
#endif

/* Polling period (must match button.c — 10 ms). */
#define FW07_POLL_PERIOD_US 10000LL

/* Runtime long-press callback counter. Registered in setUp; the
 * test asserts it stays 0 for ALL 5 boot-longpress scenarios
 * (Phase C must NOT fire the runtime cb — the runtime path is
 * gated on BUTTON_PHASE_RUNTIME which Phase D wires; the cb
 * counter must remain 0 because the BOOT_TIME phase does not
 * invoke it). */
static volatile int g_runtime_cb_count = 0;

static void runtime_cb_increment(void)
{
    g_runtime_cb_count++;
}

/* Standard fixture: reset all mocks, prime the boot-button
 * signal to false (the initial state before any boot-time
 * detection), register a runtime callback, then init the
 * button driver.
 *
 * IMPORTANT: `mock_boot_button_set(false)` primes the MOCK,
 * not the strong symbol. The strong symbol is what the test
 * asserts on (via the #undef above). */
static void setUp_button(void)
{
    mock_gpio_reset();
    mock_esp_timer_reset();
    /* Prime the mock to false so any accidental call site that
     * uses the mock sees the initial state. */
    mock_boot_button_set(false);

    g_runtime_cb_count = 0;

    /* Tear down any prior button_init() so the next init()
     * creates a fresh poll handle. */
    button_deinit();

    esp_err_t rc = button_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    rc = button_on_runtime_longpress_set(runtime_cb_increment);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
}

/* Drive a scenario: produce a sequence of polls that reflects
 * the GPIO level for the requested press pattern.
 *
 * `pressed_start_ms` is when the press begins (relative to
 * `button_init()`'s now_us = 0).
 * `pressed_end_ms` is when the press releases.
 * `total_ms` is the total simulated duration.
 *
 * Example: pressed_start=0, pressed_end=3000, total=3500 → 3 s
 * press followed by 500 ms release. */
static void drive_press(int pressed_start_ms, int pressed_end_ms,
                       int total_ms)
{
    int polls = (total_ms / 10) + 1;  /* +1 for safety margin */
    for (int i = 0; i < polls; ++i) {
        int64_t now_us = (int64_t)i * FW07_POLL_PERIOD_US;
        int now_ms = (int)(now_us / 1000);
        int level;
        if (now_ms >= pressed_start_ms && now_ms < pressed_end_ms) {
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
    "boot_longpress_3s_asserts_signal [fw-07.2]",
    "[button][fw-07.2]")
{
    setUp_button();

    /* S5: 3 s continuous press during BOOT_TIME → asserted.
     *
     * Drive: GPIO LOW from t=0 ms to t=3000 ms, then HIGH from
     * t=3000 ms to t=3500 ms. Total = 3.5 s, well within the
     * 5 s BOOT_TIME window so the rising edge happens inside
     * BOOT_TIME (not RUNTIME). The press duration = 3000 ms
     * exactly = BOOT_LONGPRESS_MS. */
    drive_press(0, 3000, 3500);

    /* Strong symbol MUST be asserted. */
    TEST_ASSERT_TRUE(boot_button_pressed_at_boot());
    /* Runtime cb MUST NOT have fired (BOOT_TIME branch only). */
    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
}

TEST_CASE(
    "boot_longpress_10s_asserts_signal_no_runtime_cb [fw-07.2]",
    "[button][fw-07.2]")
{
    setUp_button();

    /* S6: 10 s continuous press during BOOT_TIME → asserted,
     * and the runtime cb MUST NOT fire (the runtime path is
     * gated on BUTTON_PHASE_RUNTIME).
     *
     * Drive: GPIO LOW from t=0 ms to t=10000 ms. Total = 10.5 s,
     * past the 5 s BOOT_TIME window so the state machine
     * transitions to RUNTIME while the press is still held.
     * The latch MUST be asserted as soon as the press crosses
     * 3000 ms (well inside BOOT_TIME). After the transition to
     * RUNTIME, the press continues but the runtime cb MUST NOT
     * fire (Phase D is what wires the runtime cb; Phase C must
     * leave it dormant during BOOT_TIME). */
    drive_press(0, 10000, 10500);

    /* Strong symbol MUST be asserted (latched during BOOT_TIME). */
    TEST_ASSERT_TRUE(boot_button_pressed_at_boot());
    /* Runtime cb MUST have count == 0. The runtime path is
     * gated on BUTTON_PHASE_RUNTIME which Phase D wires; in
     * Phase C the RUNTIME branch is a no-op (it just keeps
     * state). */
    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
}

TEST_CASE(
    "boot_short_2s_does_not_assert [fw-07.2]",
    "[button][fw-07.2]")
{
    setUp_button();

    /* S7: 2 s boot-time press (< 3000 ms threshold) →
     * NOT asserted.
     *
     * Drive: GPIO LOW from t=0 ms to t=2000 ms, then HIGH from
     * t=2000 ms to t=2500 ms. Total = 2.5 s, well within the
     * 5 s BOOT_TIME window so the rising edge happens inside
     * BOOT_TIME. The press duration = 2000 ms < BOOT_LONGPRESS_MS
     * so the latch MUST stay false. */
    drive_press(0, 2000, 2500);

    /* Strong symbol MUST NOT be asserted. */
    TEST_ASSERT_FALSE(boot_button_pressed_at_boot());
    /* Runtime cb MUST NOT have fired. */
    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
}

TEST_CASE(
    "strap_pin_transient_500ms_is_absorbed [fw-07.2]",
    "[button][fw-07.2]")
{
    setUp_button();

    /* S8: ROM bootloader transient during STRAP_GRACE → NOT
     * asserted. The press (LOW) happens entirely during the
     * 500 ms strap-grace window and releases before the window
     * ends. Total press = 0 ms inside BOOT_TIME.
     *
     * Drive: GPIO LOW from t=0 ms to t=500 ms (covers entire
     * STRAP_GRACE window), then HIGH from t=500 ms to t=3000 ms.
     * The state machine is in STRAP_GRACE for the first 500 ms
     * (all polls return without state change). After t=500 ms,
     * phase transitions to BOOT_TIME; the GPIO is already HIGH,
     * so no falling edge is observed inside BOOT_TIME → no
     * press window opens → no latch. */
    drive_press(0, 500, 3000);

    /* Strong symbol MUST NOT be asserted. */
    TEST_ASSERT_FALSE(boot_button_pressed_at_boot());
    /* Runtime cb MUST NOT have fired. */
    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
}

TEST_CASE(
    "strap_grace_release_before_window_ends [fw-07.2]",
    "[button][fw-07.2]")
{
    setUp_button();

    /* S9: Press during STRAP_GRACE that releases before the
     * grace window ends → NOT asserted.
     *
     * Drive: GPIO LOW from t=0 ms to t=300 ms (entirely inside
     * the 500 ms STRAP_GRACE window), then HIGH from t=300 ms
     * to t=3000 ms. All polls in this scenario happen during
     * STRAP_GRACE → ignored → no state change → no latch. */
    drive_press(0, 300, 3000);

    /* Strong symbol MUST NOT be asserted. */
    TEST_ASSERT_FALSE(boot_button_pressed_at_boot());
    /* Runtime cb MUST NOT have fired. */
    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
}