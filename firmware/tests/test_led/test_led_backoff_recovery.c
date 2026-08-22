/* test_led_backoff_recovery.c — FW-06.3 host tests.
 *
 * The LED driver maps the 2 backoff/recovery states to the
 * documented blink patterns per PRD § FR-7 L231-232:
 *
 *   RECONNECT_BACKOFF → 2 s blink
 *                       (esp_timer period = 1_000_000 us)
 *   SOFT_RECOVERY     → 5 Hz rapid blink (esp_timer period = 50_000 us)
 *                       sustained 3 s via a one-shot alarm that
 *                       fires the registered recovery-complete cb
 *
 * The one-shot fire is exercised via
 * mock_esp_timer_fire_callback(handle) which synchronously
 * invokes the esp_timer_cb_t registered via esp_timer_create.
 * Mirrors FW-05's mock_httpd_invoke_registered_handler precedent
 * (FW-05 design #3616 § "Test entry: mock_esp_timer_fire_callback").
 */
#include "mock_gpio_link.h"
#include "mock_esp_timer_link.h"

#include "led.h"
#include "mock_gpio.h"
#include "mock_esp_timer.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

static void set_up_led(void)
{
    mock_gpio_reset();
    mock_esp_timer_reset();
    esp_err_t rc = led_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
}

TEST_CASE(
    "backoff_period_1000ms [fw-06.3]",
    "[led][fw-06.3]")
{
    set_up_led();

    esp_err_t rc = led_set_state(LED_STATE_RECONNECT_BACKOFF);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* RECONNECT_BACKOFF = 2 s blink → esp_timer period = 1_000_000 us
     * (half-period; the callback toggles every period). */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_periodic_call_count());
    TEST_ASSERT_EQUAL_UINT((unsigned)1000000u,
                           (unsigned)mock_esp_timer_last_period_us());
}

TEST_CASE(
    "recovery_period_50ms_and_oneshot_3000ms [fw-06.3]",
    "[led][fw-06.3]")
{
    set_up_led();

    esp_err_t rc = led_set_state(LED_STATE_SOFT_RECOVERY);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* SOFT_RECOVERY = 5 Hz rapid blink → esp_timer period = 50_000 us. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_periodic_call_count());
    TEST_ASSERT_EQUAL_UINT((unsigned)50000u,
                           (unsigned)mock_esp_timer_last_period_us());

    /* Plus the 3 s one-shot alarm for the recovery-cb. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_once_call_count());
    TEST_ASSERT_EQUAL_UINT((unsigned)3000000u,
                           (unsigned)mock_esp_timer_last_period_us_oneshot());
}

/* A trivial recovery-complete callback that flips a static flag. */
static volatile int g_recovery_flag = 0;
static void recovery_cb_set_flag(void)
{
    g_recovery_flag = 1;
}

TEST_CASE(
    "recovery_fires_callback_after_3000ms [fw-06.3]",
    "[led][fw-06.3]")
{
    set_up_led();
    g_recovery_flag = 0;

    /* Register the recovery-complete cb. */
    esp_err_t rc = led_on_recovery_complete(recovery_cb_set_flag);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Enter SOFT_RECOVERY — arms both the periodic (50_000 us) +
     * the one-shot alarm (3_000_000 us). */
    rc = led_set_state(LED_STATE_SOFT_RECOVERY);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* The one-shot handle is the 2nd handle created (index 1 in
     * the mock's slot table: 0 = periodic, 1 = one-shot). Look
     * it up and fire the callback synchronously via the
     * load-bearing test entry point. */
    TEST_ASSERT_EQUAL_INT(2, mock_esp_timer_handle_count());
    esp_timer_handle_t oneshot = mock_esp_timer_handle_at(1);
    TEST_ASSERT_NOT_NULL(oneshot);

    esp_err_t fr = mock_esp_timer_fire_callback(oneshot);
    TEST_ASSERT_EQUAL_INT(ESP_OK, fr);

    /* The recovery-complete cb MUST have fired. */
    TEST_ASSERT_EQUAL_INT(1, g_recovery_flag);
}
