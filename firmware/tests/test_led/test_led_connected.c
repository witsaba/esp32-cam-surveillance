/* test_led_connected.c — FW-06.2 host tests.
 *
 * The LED driver maps the 2 connected states to the documented
 * patterns:
 *   - CONNECTED_IDLE → LED OFF (steady state; GPIO 4 is the
 *     AI-Thinker flash LED, so no heartbeat while healthy)
 *   - STREAMING      → solid ON (no timer; level held ON)
 *
 * CONNECTED_IDLE is verified by seeding a running periodic via a
 * blink state, then asserting the transition stops the timer and
 * holds the OFF level. STREAMING asserts esp_timer_stop + ON level.
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
    /* See test_led_boot_connecting.c for rationale. */
    led_deinit();

    esp_err_t rc = led_init();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
}

TEST_CASE(
    "idle_drives_led_off [fw-06.2][power]",
    "[led][fw-06.2]")
{
    set_up_led();

    /* Seed: enter a blinking state so the periodic is running. */
    esp_err_t rc = led_set_state(LED_STATE_WIFI_CONNECTING);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
    int stop_count_before = mock_esp_timer_stop_call_count();
    int set_level_count_before = mock_gpio_set_level_call_count();
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_periodic_call_count());

    /* Transition to CONNECTED_IDLE → periodic STOPPED + GPIO level
     * held OFF (no heartbeat: GPIO 4 is the flash LED). */
    rc = led_set_state(LED_STATE_CONNECTED_IDLE);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* esp_timer_stop called at least once for the idle transition. */
    TEST_ASSERT_GREATER_THAN_INT(stop_count_before,
                                 mock_esp_timer_stop_call_count());

    /* GPIO level held OFF = level 1 under active-LOW default. */
    int pin = -1, level = -1;
    mock_gpio_set_level_capture(&pin, &level);
    TEST_ASSERT_EQUAL_INT(GPIO_NUM_4, pin);
    TEST_ASSERT_EQUAL_INT(1, level);

    /* No NEW start_periodic on the idle transition. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_periodic_call_count());
    /* gpio_set_level called once more than before (entry OFF). */
    TEST_ASSERT_EQUAL_INT(set_level_count_before + 1,
                          mock_gpio_set_level_call_count());
}

TEST_CASE(
    "streaming_stops_periodic_holds_on [fw-06.2]",
    "[led][fw-06.2]")
{
    set_up_led();

    /* Seed: enter a blinking state so the periodic is running. */
    esp_err_t rc = led_set_state(LED_STATE_WIFI_CONNECTING);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
    int stop_count_before = mock_esp_timer_stop_call_count();
    int set_level_count_before = mock_gpio_set_level_call_count();

    /* Transition to STREAMING → esp_timer_stop on the periodic +
     * GPIO level held ON (no toggle). */
    rc = led_set_state(LED_STATE_STREAMING);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* esp_timer_stop was called at least once for the streaming
     * transition. (May also be 0 if periodic was never started,
     * but our fixture set CONNECTED_IDLE first so it was started.) */
    TEST_ASSERT_GREATER_THAN_INT(stop_count_before,
                                 mock_esp_timer_stop_call_count());

    /* GPIO level held ON = level 0 under active-LOW default. */
    int pin = -1, level = -1;
    mock_gpio_set_level_capture(&pin, &level);
    TEST_ASSERT_EQUAL_INT(GPIO_NUM_4, pin);
    TEST_ASSERT_EQUAL_INT(0, level);

    /* No NEW start_periodic on the streaming transition. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_periodic_call_count());
    /* GPIO set_level called one more time than before (entry ON).
     * led_init() may have added one for the initial OFF state;
     * we just verify that the STREAMING transition called
     * gpio_set_level at least once. */
    TEST_ASSERT_GREATER_THAN_INT(set_level_count_before,
                                 mock_gpio_set_level_call_count());
}
