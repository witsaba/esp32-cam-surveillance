/* test_led_boot_connecting.c — FW-06.1 host tests.
 *
 * The LED driver maps the 3 boot/connecting states to the
 * documented blink patterns per PRD § FR-7 L226-228:
 *   - BOOTING          → solid ON (no periodic timer; level held)
 *   - WIFI_CONNECTING  → 200 ms blink  (esp_timer period = 100_000 us)
 *   - WS_CONNECTING    → 100 ms blink  (esp_timer period =  50_000 us)
 *
 * Each test calls led_init() once (creating the persistent periodic
 * + one-shot timer handles), then drives a single led_set_state(...)
 * call and asserts the mock surface observed the expected behavior.
 *
 * Conventions:
 *   - active-LOW polarity (default for AI-THINKER): ON level = 0.
 *   - GPIO 4 = CONFIG_FIRMWARE_LED_GPIO default.
 *   - The mock captures the LAST (pin, level) passed to
 *     gpio_set_level; assertions on this surface prove the entry
 *     condition was set.
 *   - The mock_esp_timer_last_period_us returns the LAST period_us
 *     passed to either esp_timer_start_periodic or esp_timer_restart;
 *     we verify it matches the expected half-period for the state.
 *
 * Tests are placed in their own file so the runner can compile
 * subsets (e.g. only this file for the FW-06.1 stub/green build).
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

/* Standard fixture: reset both mocks + init the LED driver. */
static void set_up_led(void)
{
    mock_gpio_reset();
    mock_esp_timer_reset();
    /* Tear down any prior led_init() so the next init() creates
     * fresh timer handles. Without this, back-to-back tests
     * share state and handle counts drift. */
    led_deinit();

    esp_err_t rc = led_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
}

TEST_CASE(
    "booting_holds_level_on [fw-06.1]",
    "[led][fw-06.1]")
{
    set_up_led();

    esp_err_t rc = led_set_state(LED_STATE_BOOTING);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* BOOTING = solid ON, no periodic timer. The gpio_set_level call
     * happens on entry; assert no start_periodic was issued (timer
     * stays stopped, level held). */
    int pin = -1, level = -1;
    mock_gpio_set_level_capture(&pin, &level);
    TEST_ASSERT_EQUAL_INT(GPIO_NUM_4, pin);
    /* Active-HIGH flash LED: ON → level = 1. */
    TEST_ASSERT_EQUAL_INT(1, level);

    TEST_ASSERT_EQUAL_INT(0, mock_esp_timer_start_periodic_call_count());
    /* gpio_set_level called at least once on BOOTING entry.
     * (led_init() may also call gpio_set_level once for the
     * initial OFF state, so the count is >= 1, not == 1.) */
    TEST_ASSERT_GREATER_THAN_INT(0, mock_gpio_set_level_call_count());
}

TEST_CASE(
    "wifi_connecting_period_100ms [fw-06.1]",
    "[led][fw-06.1]")
{
    set_up_led();

    esp_err_t rc = led_set_state(LED_STATE_WIFI_CONNECTING);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* WIFI_CONNECTING = 200 ms blink → esp_timer period = 100_000 us
     * (half-period; the callback toggles every period). The mock's
     * esp_timer_start_periodic_count captures the exact call. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_periodic_call_count());
    /* 100_000 fits in uint32_t (Unity's host build disables 64-bit
     * support; assert via the 32-bit cast that the mock surfaces). */
    TEST_ASSERT_EQUAL_UINT((unsigned)100000u,
                           (unsigned)mock_esp_timer_last_period_us());
}

TEST_CASE(
    "ws_connecting_period_50ms [fw-06.1]",
    "[led][fw-06.1]")
{
    set_up_led();

    esp_err_t rc = led_set_state(LED_STATE_WS_CONNECTING);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* WS_CONNECTING = 100 ms blink → esp_timer period = 50_000 us. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_periodic_call_count());
    TEST_ASSERT_EQUAL_UINT((unsigned)50000u,
                           (unsigned)mock_esp_timer_last_period_us());
}
