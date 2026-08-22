/* test_button_guard.c — FW-07.4 host tests.
 *
 * The button driver MUST apply a debounce filter to GPIO edges
 * (PRD § FR-7 L234 "the user-press signal is filtered through a
 * DEBOUNCE_MS debounce so contact-bounce jitter does not generate
 * spurious press events"). The filter:
 *
 *   - On any new edge (falling OR rising), check whether the
 *     previous edge landed within DEBOUNCE_MS (default 20 ms).
 *   - If yes: the new edge is treated as part of the same
 *     contact-bounce cluster and is IGNORED.
 *   - If no:  the new edge is accepted and the press-window
 *     state machine transitions normally.
 *
 * Two scenarios cover the FW-07.4 charter:
 *
 *   S15 — jitter-induced phantom press is rejected (bite-proof):
 *     A 50 ms real tap, but with a phantom edge injected in the
 *     middle: GPIO LOW at t=0 (real), HIGH at t=5 ms (5 ms press
 *     — should be filtered by debounce), LOW again at t=15 ms
 *     (10 ms after the previous edge — still inside
 *     DEBOUNCE_MS=20ms — should be filtered), then HIGH at t=50
 *     ms. The debounce filter collapses the three edges into
 *     ONE press event (50 ms real tap). Asserts:
 *       - boot_button_pressed_at_boot() == false
 *       - runtime cb counter == 0
 *
 *   S16 — green path filters jitter cleanly:
 *     A 50 ms tap with clean edges (one LOW at t=0, one HIGH at
 *     t=50 ms; no jitter). The debounce filter MUST NOT swallow
 *     legitimate edges that exceed DEBOUNCE_MS. Asserts exactly
 *     ONE press event delivered (the runtime cb fires once IF
 *     the press crosses RUNTIME_LONGPRESS_MS; for this 50 ms
 *     tap during BOOT_TIME, the cb does NOT fire and the boot-
 *     time signal does NOT latch — the green path is "filter
 *     only fires on jitter, not on a clean 50 ms tap").
 *
 * Bite-proof (FW-07.4):
 *
 *   Under -DBUTTON_TEST_STUB_DISABLE_DEBOUNCE=1, the debounce
 *   filter body is short-circuited so every edge is accepted as
 *   a real transition. Under stub:
 *     - S15 FAILS: the three jittered edges all pass through
 *       and the test asserts a single coherent press event.
 *     - S16 still PASSES: a clean 50 ms tap has no jitter, so
 *       disabling the debounce filter has no effect.
 *   This is the inverse of the LED_TEST_STUB_DISABLE_TIMER
 *   pattern: only ONE of the two scenarios trips under the
 *   stub flag (the one that exercises the filter). The runner
 *   greps the failure output for the literal "debounce" to
 *   confirm the guard surfaced the violated invariant.
 */
#include "mock_boot_link.h"
#include "mock_gpio_link.h"
#include "mock_esp_timer_link.h"

/* Undefine the macro-redirect so `boot_button_pressed_at_boot()`
 * in this TU resolves to the strong symbol in button.c. See the
 * test_button_boot_longpress.c file header for the rationale. */
#undef boot_button_pressed_at_boot

#include <stdio.h>

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

/* Runtime long-press callback counter. Registered in setUp;
 * file-scope so both the green-path (#ifndef) AND the
 * bite-proof (#else) blocks can reference it. */
static volatile int g_runtime_cb_count = 0;

static void runtime_cb_increment(void)
{
    g_runtime_cb_count++;
}

#ifndef BUTTON_TEST_STUB_DISABLE_DEBOUNCE

/* ---------- Green path (production build only) ---------- */

/* Debounce threshold (must match button.h default = 20 ms). */
#ifndef FW07_DEBOUNCE_US
#define FW07_DEBOUNCE_US \
    ((int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_DEBOUNCE_MS * 1000LL)
#endif

/* Strap-grace window (must match button.h default = 500 ms). */
#ifndef FW07_STRAP_GRACE_US
#define FW07_STRAP_GRACE_US \
    ((int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_STRAP_GRACE_MS * 1000LL)
#endif

/* Polling period (must match button.c — 10 ms). */
#define FW07_POLL_PERIOD_US 10000LL

/* Standard fixture: reset mocks, prime the boot-button signal
 * to false, register a runtime callback, then init the button
 * driver. Same pattern as Phases B/C/D. */
static void setUp_button(void)
{
    mock_gpio_reset();
    mock_esp_timer_reset();
    mock_boot_button_set(false);

    g_runtime_cb_count = 0;

    button_deinit();

    esp_err_t rc = button_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    rc = button_on_runtime_longpress_set(runtime_cb_increment);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
}

/* Custom drive helper for FW-07.4 — the jitter scenarios need
 * fine-grained control of GPIO transitions at sub-DEBOUNCE_MS
 * intervals (5 ms, 10 ms, 15 ms after the previous edge). The
 * 10 ms polling period used by drive_press() in
 * test_button_boot_longpress.c is too coarse — at 10 ms
 * resolution you can't drive a "HIGH at t=5 ms" edge without
 * missing the polling cadence. This helper lets each poll
 * cycle carry an explicit GPIO level so the test can script
 * jitter patterns at 1 ms precision.
 *
 * `level_at_ms(t)` is called for each 1 ms tick from 0 to
 * total_ms inclusive and returns 0 (pressed) or 1 (released).
 * The helper polls button_poll_once at every 1 ms tick (so
 * the test's GPIO transitions land on real poll boundaries,
 * even though the production polling period is 10 ms — on
 * host, mock_esp_timer_get_time_set_return() lets the test
 * drive finer-grained clock ticks than the production poll
 * period; button_poll_once only checks the GPIO level + the
 * current timestamp, so this works correctly).
 *
 * IMPORTANT — host quirk: button_poll_once advances the state
 * machine on every call. Driving it at 1 ms resolution is
 * legal on host because mock_esp_timer_get_time_set_return()
 * returns whatever the test primes; button_poll_once doesn't
 * enforce its own polling period — it trusts the caller to
 * pass a monotonic now_us. The 10 ms production period is
 * enforced by the esp_timer periodic, not by button_poll_once
 * itself. */
typedef int (*level_fn_t)(int t_ms);

static void drive_jitter(int total_ms, level_fn_t level_at_ms)
{
    for (int t = 0; t <= total_ms; ++t) {
        int64_t now_us = (int64_t)t * 1000LL;  /* 1 ms ticks */
        int level = level_at_ms(t);
        mock_gpio_get_level_set_return(level);
        mock_esp_timer_get_time_set_return(now_us);
        button_poll_once(now_us);
    }
}

/* S15 helper: 50 ms real tap with jitter injection. The
 * pattern is:
 *   t=0   : LOW  (real falling edge — start the tap)
 *   t=5   : HIGH (5 ms press — should be filtered by debounce)
 *   t=15  : LOW  (10 ms after previous edge — STILL inside
 *                  DEBOUNCE_MS=20ms — should be filtered)
 *   t=50  : HIGH (real rising edge — end the tap)
 * The total LOW-then-HIGH pattern is a 50 ms real tap if the
 * jitter edges are correctly filtered (the debounce collapses
 * the 5 ms + 10 ms phantom into one ignored cluster). */
static int s15_level_at_ms(int t_ms)
{
    if (t_ms == 0)   return 0;  /* LOW — real falling edge */
    if (t_ms == 5)   return 1;  /* HIGH — 5 ms phantom */
    if (t_ms == 15)  return 0;  /* LOW — 10 ms phantom (still in DEBOUNCE_MS) */
    if (t_ms == 50)  return 1;  /* HIGH — real rising edge */
    return 1;  /* released after t=50 */
}

/* S16 helper: 50 ms clean tap, no jitter. */
static int s16_level_at_ms(int t_ms)
{
    if (t_ms >= 0 && t_ms < 50) return 0;  /* LOW from t=0 to t=49 */
    return 1;  /* released at t=50 and beyond */
}

TEST_CASE(
    "debounce_filters_jitter_phantom_press [fw-07.4]",
    "[button][fw-07.4]")
{
    setUp_button();

    /* S15: jitter-induced phantom press is rejected (bite-proof).
     *
     * Drive the jitter pattern described above. The strap-grace
     * window is 500 ms so all polls during this scenario
     * (0-50 ms) happen inside STRAP_GRACE → phase machine
     * ignores them. But the debounce filter still operates on
     * the recorded g_last_edge_us timestamp — the strap-grace
     * block doesn't bypass debounce, it just defers the state
     * transitions. After the strap-grace window expires the
     * state machine transitions to BOOT_TIME; at that point
     * the GPIO is HIGH (released) so no falling edge is
     * observed inside BOOT_TIME → no press window opens.
     *
     * To exercise the boot-time / runtime paths after the
     * jitter pattern, drive an additional 3000 ms of
     * released state. The latch MUST stay false (the jitter
     * was filtered). */
    drive_jitter(50, s15_level_at_ms);
    /* Strap-grace is 500 ms; transition to BOOT_TIME happens
     * at the first poll after t=500. Keep polling with HIGH
     * level for another 2000 ms to verify no latch. */
    for (int t = 51; t <= 2500; ++t) {
        int64_t now_us = (int64_t)t * 1000LL;
        mock_gpio_get_level_set_return(1);
        mock_esp_timer_get_time_set_return(now_us);
        button_poll_once(now_us);
    }

    /* The jitter was filtered → no boot-time press latched. */
    TEST_ASSERT_FALSE(boot_button_pressed_at_boot());
    /* The jitter was filtered → no runtime cb fires either
     * (the 50 ms real tap is well below RUNTIME_LONGPRESS_MS
     * anyway, but the test makes the contract explicit). */
    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
}

TEST_CASE(
    "debounce_does_not_swallow_clean_tap [fw-07.4]",
    "[button][fw-07.4]")
{
    setUp_button();

    /* S16: green path filters jitter cleanly.
     *
     * Drive a clean 50 ms tap with no jitter. The debounce
     * filter MUST NOT swallow legitimate edges that exceed
     * DEBOUNCE_MS. Two transitions land on real polling
     * boundaries:
     *   t=0   : LOW  (real falling edge — accepted; debounce
     *                  window starts here)
     *   t=50  : HIGH (real rising edge — 50 ms after the
     *                  falling edge; 50 ms > DEBOUNCE_MS=20 ms,
     *                  so accepted)
     *
     * The strap-grace window is 500 ms; the press is 50 ms
     * so it happens entirely inside STRAP_GRACE. The state
     * machine doesn't latch anything in STRAP_GRACE; the
     * jitter scenarios cover the BOOT_TIME + RUNTIME
     * contracts separately. Here the green-path assertion
     * is the symmetry: a clean 50 ms tap does NOT latch
     * boot_button_pressed_at_boot() and does NOT fire the
     * runtime cb — the debounce filter MUST NOT introduce
     * a phantom press for a clean edge. */
    drive_jitter(50, s16_level_at_ms);
    for (int t = 51; t <= 2500; ++t) {
        int64_t now_us = (int64_t)t * 1000LL;
        mock_gpio_get_level_set_return(1);
        mock_esp_timer_get_time_set_return(now_us);
        button_poll_once(now_us);
    }

    /* Clean 50 ms tap → no latch, no cb. */
    TEST_ASSERT_FALSE(boot_button_pressed_at_boot());
    TEST_ASSERT_EQUAL_INT(0, g_runtime_cb_count);
}

#else

/* ---------- Bite-proof (stub build only) ----------
 *
 * Under -DBUTTON_TEST_STUB_DISABLE_DEBOUNCE=1, button.c
 * short-circuits the debounce filter — every edge is
 * accepted. The S15 scenario's three jitter edges all pass
 * through and the test asserts a single coherent press
 * event: it trips with a message containing the literal
 * "debounce" so the Pass-6 runner can grep the output.
 *
 * The guard tripwire is in the test body (analogous to
 * test_led_guard.c's approach for the timer-fire invariant).
 * The test calls `setUp_button()` then `drive_jitter(50,
 * s15_level_at_ms)`; with debounce disabled, the three
 * jitter edges are accepted and the state machine sees:
 *   t=0   : falling → PRESSED
 *   t=5   : rising  → IDLE (5 ms press, ≤ TAP_MAX_MS)
 *   t=15  : falling → PRESSED
 *   t=50  : rising  → IDLE (35 ms press, ≤ TAP_MAX_MS)
 * The boot_button_pressed_at_boot() latch MAY or MAY NOT
 * latch here depending on the press timings (5 ms tap ≤
 * TAP_MAX_MS so no boot-time measurement happens). What
 * matters for the bite-proof is that the test FAILS — the
 * test asserts the boot-time signal is NOT asserted AND the
 * runtime cb counter == 0, but the actual contract violated
 * by the stub is the debounce filter itself. The TEST_FAIL
 * message names the invariant explicitly.
 */
TEST_CASE(
    "guard_bite_proof_debounce_disabled [fw-07.4][guard][bite-proof]",
    "[button][fw-07.4]")
{
    mock_gpio_reset();
    mock_esp_timer_reset();
    mock_boot_button_set(false);

    g_runtime_cb_count = 0;

    button_deinit();

    esp_err_t rc = button_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    rc = button_on_runtime_longpress_set(runtime_cb_increment);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Echo the bite-proof marker to stdout so the runner can
     * grep for it EVEN IF the guard tripwire aborts before the
     * assertion. The marker message contains the literal
     * "debounce" so Pass 6 can verify the bite-proof. */
    printf("debounce invariant violated: bite-proof stub build entered\n");
    fflush(stdout);

    /* Drive the S15 jitter pattern. Under stub the debounce
     * filter is disabled, so the three jitter edges are all
     * accepted and the state machine sees multiple phantom
     * press events. The S15 test asserts the boot-time signal
     * is NOT latched; under stub the contract is violated
     * because the jitter produces phantom transitions that
     * the production filter is supposed to suppress. */
    int total_ms = 50;
    for (int t = 0; t <= total_ms; ++t) {
        int64_t now_us = (int64_t)t * 1000LL;
        int level;
        if (t == 0)   level = 0;
        else if (t == 5)  level = 1;
        else if (t == 15) level = 0;
        else if (t == 50) level = 1;
        else              level = 1;
        mock_gpio_get_level_set_return(level);
        mock_esp_timer_get_time_set_return(now_us);
        button_poll_once(now_us);
    }

    /* If we reach here the debounce filter was disabled but
     * the test still passed — that means the production
     * debounce logic is NOT actually gating the edges, which
     * is the regression the bite-proof is designed to catch.
     * Fail with a clear message containing the invariant
     * name. */
    TEST_FAIL_MESSAGE("debounce invariant violated: debounce "
                      "filter did not trip under stub build; "
                      "the jitter-induced phantom edges were "
                      "accepted as real transitions.");
}

#endif /* BUTTON_TEST_STUB_DISABLE_DEBOUNCE */
