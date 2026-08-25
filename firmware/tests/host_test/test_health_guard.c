/* test_health_guard.c — FW-16.3 healthy-stream bite-proof guard
 * (R-FW16-1.3; design AD7 / Pass 13 spec).
 *
 * This file compiles in TWO builds:
 *
 *   - PRODUCTION (Pass 1, no flags): the tick calls below are
 *     excluded by #ifndef — the test then pins the green path
 *     end-to-end: 60 s of simulated healthy connected streaming
 *     through the REAL event entry points leaves ZERO recovery
 *     artifacts (no NVS write, un-armed LED, no restart).
 *
 *   - STUB BUILD (Pass 13, -DHEALTH_TEST_STUB_COUNT_WHILE_HEALTHY=1,
 *     applied to BOTH the health production sources AND this file):
 *     health_green_path_tick_for_guard() exists and records ONE
 *     phantom failure per tick with NO wifi event and NO episode
 *     latch — a model of a future always-sweeping miscounting
 *     implementation. Sixty ticks at 1 Hz exceed the default
 *     threshold of 30, so the assertions below MUST FAIL with the
 *     literal "healthy-stream" in the message. Pass 13 greps for
 *     exactly that single expected failure.
 */

#include <stddef.h>
#include <stdint.h>

#include "health.h"

#include "mock_nvs_flash.h"
#include "mock_esp_system.h"
#include "mock_log.h"

#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_timer_link.h"

#include "wifi.h"
#include "wifi_event.h"
#include "led.h"

#include "mock_gpio.h"

#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

static void guard_fixture(void)
{
    led_deinit(); /* fresh LED handles after the mock reset */

    mock_esp_event_reset();
    mock_esp_timer_reset();
    mock_esp_system_reset();
    mock_nvs_reset();
    mock_log_reset();
    mock_gpio_reset();

    TEST_ASSERT_EQUAL(ESP_OK, led_init());
    TEST_ASSERT_EQUAL(ESP_OK, health_task_start());
}

TEST_CASE(
    "test_health_guard_60s_healthy_stream_must_not_count [fw-16.3][guard][bite-proof]",
    "[health][fw-16.3][guard]")
{
    guard_fixture();

#ifdef HEALTH_TEST_STUB_COUNT_WHILE_HEALTHY
    /* ---- Pass 13 stub build: the bite target is live. ---- */
    for (int sec = 0; sec < 60; ++sec) {
        mock_esp_timer_get_time_set_return((int64_t)sec * 1000000LL);
        health_green_path_tick_for_guard();
    }

    /* A sweeping implementation that miscounts healthy seconds MUST
     * trip this message — the literal "healthy-stream" is what
     * Pass 13 greps for. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, (int)health_guard_failure_count(),
        "healthy-stream invariant violated: failure counter advanced "
        "during healthy connected streaming");
    TEST_ASSERT_FALSE_MESSAGE(
        health_guard_should_recover(),
        "healthy-stream invariant violated: soft recovery would fire "
        "during healthy connected streaming");
#else
    /* ---- Production build: pin the real green path. ---- */
    mock_esp_timer_get_time_set_return(0);
    (void)mock_esp_event_fire_handler(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    for (int sec = 1; sec <= 60; ++sec) {
        mock_esp_timer_get_time_set_return((int64_t)sec * 1000000LL);
        (void)mock_esp_event_fire_handler(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          NULL);
    }

    /* Event-driven-only evaluation: healthy streaming produces NO
     * counted failures, hence zero recovery artifacts. If any of
     * these trip, an always-sweeping regression slipped in. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, mock_nvs_write_count(),
        "healthy-stream invariant violated: recovery reason persisted "
        "during healthy streaming");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, mock_esp_timer_start_once_call_count(),
        "healthy-stream invariant violated: SOFT_RECOVERY LED armed "
        "during healthy streaming");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, mock_esp_restart_call_count(),
        "healthy-stream invariant violated: restart during healthy "
        "streaming");
#endif
}
