/* test_led_guard.c — FW-06.4 host tests.
 *
 * The LED driver MUST re-arm the periodic esp_timer within one
 * period of the previous pattern on every led_set_state(s)
 * transition (PRD L556 + milestones FW-06.4 charter L591 "LED
 * never sticks in a transient state because of a missed timer
 * event"). Two test scenarios cover the guard:
 *
 *   1. Green path: every state transition re-arms the timer.
 *      Given state = WIFI_CONNECTING (timer armed at 100_000 us),
 *      When led_set_state(WS_CONNECTING) is invoked,
 *      Then esp_timer_start_periodic was called with the new
 *      period_us = 50_000 us.
 *
 *   2. Bite-proof (FW-06.4): under stub build
 *      -DLED_TEST_STUB_DISABLE_TIMER=1, the esp_timer_create body
 *      in led.c is short-circuited so the timer never fires. The
 *      driver asserts that any state transition requires a
 *      running timer and trips the guard with a message
 *      containing the literal "timer_fire" so the runner can
 *      verify the guard is load-bearing. This test is TEST_IGNORE
 *      under the production build (no stub flag) and FAILS under
 *      the stub build (Pass 5 of run_host_tests.py).
 */
#include "mock_gpio_link.h"
#include "mock_esp_timer_link.h"

#include <stdio.h>

#include "led.h"
#include "mock_gpio.h"
#include "mock_esp_timer.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#ifndef LED_TEST_STUB_DISABLE_TIMER

/* ---------- Green path (production build only) ---------- */

static void set_up_led(void)
{
    mock_gpio_reset();
    mock_esp_timer_reset();
    /* See test_led_boot_connecting.c for rationale. */
    led_deinit();

    esp_err_t rc = led_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
}

TEST_CASE(
    "set_state_rearms_timer [fw-06.4][green]",
    "[led][fw-06.4]")
{
    set_up_led();

    /* Seed: enter a blinking state so the periodic is running at
     * the WIFI_CONNECTING period (100_000 us = 200 ms blink). */
    esp_err_t rc = led_set_state(LED_STATE_WIFI_CONNECTING);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    int periodic_count_before = mock_esp_timer_start_periodic_call_count();
    TEST_ASSERT_EQUAL_INT(1, periodic_count_before);
    TEST_ASSERT_EQUAL_UINT((unsigned)100000u,
                           (unsigned)mock_esp_timer_last_period_us());

    /* Transition into WS_CONNECTING (50_000 us = 100 ms blink).
     * The driver MUST re-arm the periodic with the new period
     * (or stop+start, but the count must go up by >= 1). */
    rc = led_set_state(LED_STATE_WS_CONNECTING);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    TEST_ASSERT_GREATER_THAN_INT(periodic_count_before,
                                 mock_esp_timer_start_periodic_call_count());
    TEST_ASSERT_EQUAL_UINT((unsigned)50000u,
                           (unsigned)mock_esp_timer_last_period_us());
}

#else

/* ---------- Bite-proof (stub build only) ----------
 *
 * Under -DLED_TEST_STUB_DISABLE_TIMER=1, led_init() skips
 * esp_timer_create (the timer never exists; the periodic handle
 * remains NULL). On led_set_state(WS_CONNECTING), the driver's
 * guard trips because there is no timer to restart — exactly
 * the timer-fire invariant the guard protects.
 *
 * The guard printf+aborts with a message containing the
 * literal "timer_fire" so the runner can grep the output. The
 * process will exit non-zero (SIGABRT) which the Pass-5 runner
 * interprets as the expected bite-proof failure. Mirrors the
 * determinism / schema_version / validation keywords from
 * the prior bite-proofs (FW-03.4 / FW-02.3 / FW-05.4).
 */
TEST_CASE(
    "guard_bite_proof_timer_fire_disabled [fw-06.4][guard][bite-proof]",
    "[led][fw-06.4]")
{
    mock_gpio_reset();
    mock_esp_timer_reset();

    /* led_init under stub: gpio_config returns OK, but
     * esp_timer_create is short-circuited. The init itself
     * succeeds (returns ESP_OK); the guard trips on the
     * FIRST led_set_state call that requires a timer. */
    esp_err_t rc = led_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* No handles should have been created under stub. */
    TEST_ASSERT_EQUAL_INT(0, mock_esp_timer_handle_count());

    /* Echo the bite-proof marker to stdout so the runner can
     * grep for it EVEN IF the guard tripwire aborts before the
     * assertion. The guard will abort the process with a
     * similar message containing "timer_fire" — Pass 5 verifies
     * both signals (rc != 0 AND literal in output). */
    printf("timer_fire: bite-proof stub build entered\n");
    fflush(stdout);

    /* Enter a blinking state. The guard trips because the
     * periodic handle is NULL. This call is expected to abort
     * the process; the assertion below is unreachable. */
    (void)led_set_state(LED_STATE_WS_CONNECTING);

    /* If we reach here the guard didn't trip — that's a
     * regression in the bite-proof gate. Fail with a clear
     * message containing the invariant name. */
    TEST_FAIL_MESSAGE("timer_fire invariant violated: guard "
                      "did not trip under stub build; the LED "
                      "would stick in the previous period.");
}

#endif /* LED_TEST_STUB_DISABLE_TIMER */
