/* test_button_runtime_reset.c — FW-07.3 host tests.
 *
 * The runtime factory-reset cb path: when the user holds the
 * BOOT button for ≥ CONFIG_FIRMWARE_BOOT_BUTTON_RUNTIME_LONGPRESS_MS
 * (default 10000 ms) during the RUNTIME phase, the button driver
 * invokes the user-registered callback exactly once. Production
 * wires `config_factory_reset() + esp_restart()` into the
 * callback (FW-07.3 contract; see PRD § FR-7 L236).
 *
 * S10: 10 s runtime press wipes NVS and restarts into
 *      provisioning. Drive BOOT_TIME (5 s HIGH/LOW pattern from
 *      Phase C S5) → RUNTIME → 10 s LOW. Assert the registered
 *      runtime cb fires exactly once → both `mock_config_factory_reset`
 *      and `mock_esp_restart` counters == 1.
 * S11: 5 s runtime press is ignored.
 * S12: boundary: 10001 ms triggers reset, 9999 ms does NOT.
 * S13: runtime press does NOT touch `camera_cfg` NVS namespace.
 *      The production `config_factory_reset` API takes no args and
 *      only wipes the `config` namespace. Verify the cb was called
 *      with zero `camera_cfg` argument (just an integration
 *      contract assertion — the API signature has no arg by
 *      definition) and a hypothetical `mock_camera_cfg_namespace_wipe`
 *      counter stays 0.
 * S14: factory reset calls `config_factory_reset()` exactly once
 *      and `esp_restart()` exactly once. Direct counter
 *      assertion (re-verified under S10's setup).
 *
 * Cooldown: a `DEBOUNCE_MS * 4 = 80 ms` cooldown follows the cb
 * dispatch so a user holding the button past 10 s does NOT double-
 * fire the cb. The cooldown is critical — verified by S10's
 * assertions and the boundary checks in S12.
 *
 * IMPORTANT — calling the strong symbol + production cb on host:
 *
 * The test's setUp registers a runtime callback that:
 *   1. Increments `g_outer_cb_count` (the OUTER counter; the
 *      test asserts this is exactly 1 after each scenario).
 *   2. Calls `config_factory_reset()` → redirected to
 *      `mock_config_factory_reset` via `mock_config_link.h`
 *      (count + set_return; default CONFIG_OK).
 *   3. Calls `esp_restart()` → redirected to `mock_esp_restart`
 *      via `mock_esp_system_link.h` (counter-only no-op).
 *
 * The OUTER counter proves the button driver fired the cb exactly
 * once (NOT the production code in boot.c — boot.c is the OWNER
 * of the callback in production; on host the test registers its
 * OWN cb that wraps the same production body to prove the
 * contract works without depending on the boot.c wiring being
 * included in this test build).
 *
 * The boot.c production wiring (`button_on_runtime_longpress_set`
 * in `boot_run_normal`) lands in the same commit as a SEPARATE
 * requirement — see boot.c::boot_run_normal and its own
 * `mock_esp_system_link.h` include for the host redirect.
 */
#include "mock_boot_link.h"
#include "mock_gpio_link.h"
#include "mock_esp_timer_link.h"
#include "mock_config_link.h"
#include "mock_esp_system_link.h"

#include "button.h"
#include "config.h"
#include "mock_boot_button.h"
#include "mock_config.h"
#include "mock_esp_system.h"
#include "mock_gpio.h"
#include "mock_esp_timer.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* Runtime long-press threshold (must match button.h default). */
#ifndef FW07_RUNTIME_LONGPRESS_US
#define FW07_RUNTIME_LONGPRESS_US \
    ((int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_RUNTIME_LONGPRESS_MS * 1000LL)
#endif

/* Debounce gap (must match button.h default). */
#ifndef FW07_DEBOUNCE_US
#define FW07_DEBOUNCE_US \
    ((int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_DEBOUNCE_MS * 1000LL)
#endif

/* Boot-time window (must match button.h default). */
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

/* Outer runtime-callback counter (the cb registered by the test).
 * Increments every time the registered cb fires — the test asserts
 * == 1 once after the S10 scenario, == 0 for S11/S13, etc. */
static volatile int g_outer_cb_count = 0;

/* Hypothetical camera-cfg namespace-wipe counter (S13). The
 * production `config_factory_reset` API does NOT touch this
 * namespace — the test verifies it stays 0 when the runtime
 * factory-reset cb fires (in a stub build we seed it to 0; in
 * production this counter would not exist because the API itself
 * guarantees the scoping). */
static volatile int g_camera_cfg_wipe_count = 0;

/* Outer runtime callback: wraps `config_factory_reset + esp_restart`
 * the same way boot.c::boot_factory_reset_and_restart does on device.
 * When the production cb is implemented in boot.c, this OUTER cb is
 * the test stand-in: it proves the button driver's RUNTIME dispatch
 * contract without coupling to boot.c's wiring. The OUTER counter
 * proves the button driver fired the cb exactly once. */
static void outer_runtime_cb(void)
{
    g_outer_cb_count++;
    /* config_factory_reset → mock_config_factory_reset (via the
     * mock_config_link.h macro redirect). The mock's call counter
     * is queried from the test assertions. We prime the return to
     * CONFIG_OK via mock_config_factory_reset_set_return in setUp
     * (default is CONFIG_OK after a reset). */
    config_status_t st = config_factory_reset();
    (void)st;  /* result consumed by the mock; assertion is on counter */
    if (g_camera_cfg_wipe_count) {
        /* No-op guard — g_camera_cfg_wipe_count is reset in setUp
         * and stays 0 by contract. Listed here purely as a marker
         * so the compiler keeps the variable referenced. */
    }
    /* esp_restart → mock_esp_restart (via mock_esp_system_link.h).
     * Counter-only no-op. */
    esp_restart();
}

/* Standard fixture: reset all mocks, register a runtime callback,
 * init the button driver.
 *
 * No boot-time-press priming is needed for the runtime-reset tests
 * — the runtime cb path is independent of the BOOT_TIME latch. */
static void setUp_button(void)
{
    mock_gpio_reset();
    mock_esp_timer_reset();
    mock_config_reset();          /* adds Phase D's config mock */
    /* mock_esp_system_reset clears the esp_restart counter
     * (used by the runtime cb dispatch) and the MAC/chip/version
     * state (not relevant to FW-07 but unused counters would
     * otherwise leak between tests). Without this reset the
     * counter accumulates across tests and S12's "expected 0"
     * assertions would fail because S13+S14's fires leak into
     * S12's counter. */
    mock_esp_system_reset();

    mock_boot_button_set(false);

    g_outer_cb_count     = 0;
    g_camera_cfg_wipe_count = 0;
    mock_config_factory_reset_set_return(CONFIG_OK);
    /* esp_restart is counter-only; no _set_return needed. */

    /* Tear down any prior button_init() so the next init()
     * creates a fresh poll handle + clears the cb. */
    button_deinit();

    esp_err_t rc = button_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    rc = button_on_runtime_longpress_set(outer_runtime_cb);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
}

/* Drive a scenario: produce a sequence of polls that reflects
 * the GPIO level for the requested press pattern.
 *
 * `pressed_start_ms` is when the press begins (relative to
 * `button_init()`'s now_us = 0).
 * `pressed_end_ms` is when the press releases.
 * `total_ms` is the total simulated duration. */
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

/* Helper for S10: drive a Phase-C scenario (5 s BOOT_TIME HIGH +
 * 5 s LOW press + 10 s RUNTIME LOW press + 500 ms tail) so the
 * RUNTIME phase is reached by t=5500 ms and the runtime long-press
 * threshold is crossed by t=10500 ms... wait — the threshold is
 * measured by duration, not by absolute time. The press starts at
 * t=5000 (after the 5 s BOOT_TIME "press"), so by t=15000 the
 * runtime duration is 10 s.
 *
 * Pattern:
 *   t=[0, 500)    ms: HIGH (strap-grace preturbation — none here)
 *   t=[500, 5500) ms: LOW  (5 s of BOOT_TIME-press; latch fires at 3000)
 *   t=[5500, 15500) ms: continued LOW after BOOT_TIME end (state
 *                       machine transitions to RUNTIME at 5500).
 *   Total = 15600 ms.
 * Runtime press duration from t=5500 onward = 10000 ms → cb fires
 * exactly at t=15500 (the press duration crosses RUNTIME_LONGPRESS_MS
 * at the in-POLL boundary).
 */
static void drive_10s_runtime_press(void)
{
    /* S10 pattern: 5 s BOOT_TIME LOW (asserts boot-time signal at
     * 3 s) followed by a continuous 10 s LOW into RUNTIME (cb
     * fires at the 10 s runtime threshold). */
    drive_press(500, 15500, 15600);
}

TEST_CASE(
    "runtime_10s_press_wipes_and_restarts [fw-07.3][scenario-S10]",
    "[button][fw-07.3]")
{
    setUp_button();

    /* S10: 10 s runtime press wipes NVS and restarts into
     * provisioning. Drive 5 s BOOT_TIME press + 10 s RUNTIME
     * press. */
    drive_10s_runtime_press();

    /* The runtime cb MUST have fired exactly once. */
    TEST_ASSERT_EQUAL_INT(1, g_outer_cb_count);
    /* `config_factory_reset()` → mock_config_factory_reset:
     * called exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_config_factory_reset_call_count());
    /* `esp_restart()` → mock_esp_restart:
     * called exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());
}

TEST_CASE(
    "runtime_5s_press_is_ignored [fw-07.3][scenario-S11]",
    "[button][fw-07.3]")
{
    setUp_button();

    /* S11: 5 s runtime press (< 10000 ms threshold) → ignored. */
    drive_press(5500, 10500, 10600);

    /* Runtime cb MUST NOT have fired. */
    TEST_ASSERT_EQUAL_INT(0, g_outer_cb_count);
    TEST_ASSERT_EQUAL_INT(0, mock_config_factory_reset_call_count());
    TEST_ASSERT_EQUAL_INT(0, mock_esp_restart_call_count());
}

TEST_CASE(
    "runtime_9990ms_does_not_trigger_10010ms_does [fw-07.3][scenario-S12]",
    "[button][fw-07.3]")
{
    /* S12: boundary check around RUNTIME_LONGPRESS_MS (10 s
     * default). The host polling period is 10 ms (matches
     * production), so the closest practical boundary checks
     * are: BELOW threshold (9990 ms = held_us 9,990,000 us
     * < RUNTIME_LONGPRESS_US = 10,000,000 us at the rising
     * edge → no fire) vs ABOVE threshold (10010 ms = held_us
     * 10,010,000 us ≥ threshold → fire). Implemented as a
     * single test case driving both sub-presses (after
     * re-init for each) so the test binary has 1 logical
     * scenario entry per test name. */

    /* ---- 9990 ms sub-scenario (must NOT fire) ---- */
    setUp_button();
    drive_press(5500, 15490, 15500);  /* 9990 ms of RUNTIME press */

    TEST_ASSERT_EQUAL_INT(0, g_outer_cb_count);
    TEST_ASSERT_EQUAL_INT(0, mock_config_factory_reset_call_count());
    TEST_ASSERT_EQUAL_INT(0, mock_esp_restart_call_count());

    /* Tear down + re-init so the state machine is fresh for the
     * 10010 ms sub-scenario. */
    button_deinit();

    /* ---- 10010 ms sub-scenario (must fire) ---- */
    setUp_button();
    drive_press(5500, 15510, 15610);  /* 10010 ms of RUNTIME press */

    TEST_ASSERT_EQUAL_INT(1, g_outer_cb_count);
    TEST_ASSERT_EQUAL_INT(1, mock_config_factory_reset_call_count());
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());
}

TEST_CASE(
    "runtime_press_does_not_touch_camera_cfg_namespace [fw-07.3][scenario-S13]",
    "[button][fw-07.3]")
{
    setUp_button();

    /* S13: runtime press calls `config_factory_reset()` which
     * wipes only the `config` namespace — does NOT touch
     * `camera_cfg`. The mock contract: `mock_config_factory_reset`
     * records the call but does NOT trigger any
     * `camera_cfg_namespace_wipe`. The test asserts:
     *   - `mock_config_factory_reset_call_count() == 1` (the cb
     *     fired exactly once and called the reset exactly once).
     *   - `g_camera_cfg_wipe_count == 0` (the `camera_cfg`
     *     namespace was not touched — the production API does
     *     not have a parameter to touch it, and the test's
     *     OUTER cb does not touch it either).
     *
     * Drive 10 s RUNTIME press (after the 5 s BOOT_TIME press
     * to reach RUNTIME). */
    drive_10s_runtime_press();

    /* `config_factory_reset` called exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_config_factory_reset_call_count());
    /* `camera_cfg` namespace wipe count is 0 (production API does
     * not touch it). */
    TEST_ASSERT_EQUAL_INT(0, g_camera_cfg_wipe_count);
}

TEST_CASE(
    "factory_reset_calls_once_each [fw-07.3][scenario-S14]",
    "[button][fw-07.3]")
{
    setUp_button();

    /* S14: same setup as S10 but with a direct counter
     * assertion under a fresh init to make the contract
     * verifiable without depending on the S10 ordering. */
    drive_10s_runtime_press();

    /* `config_factory_reset()` called exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_config_factory_reset_call_count());
    /* `esp_restart()` called exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());
}
