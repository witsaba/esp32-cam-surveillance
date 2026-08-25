/* test_soft_recovery_trigger.c — FW-16.1 trigger-sequence ordering +
 * FW-16.3 healthy-stream green path (R-FW16-1.1/1.3; design AD5/AD6).
 *
 * S1 pins the STRICT sequence on threshold crossing:
 *   persist reason (NVS ns "recovery") → led_set_state
 *   (LED_STATE_SOFT_RECOVERY arms the 3 s one-shot) → …one-shot
 *   fires… → registered completion cb → esp_restart().
 * Everything up to the one-shot fire is synchronous inside the
 * DISCONNECTED handler, so the intermediate state after the 30th
 * drop proves steps ① and ② ran while ④ has NOT yet: reason
 * readable, one-shot armed, restart counter still zero. Firing the
 * one-shot handle then drives ③→④ deterministically.
 *
 * G1 pins the event-driven-only invariant: 60 s of simulated healthy
 * connected streaming (GOT_IP cadence, clock advanced via the primed
 * µs mock) leaves ZERO persisted writes, an un-armed LED, and no
 * restart — there is no periodic sweep in production.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include <nvs.h>

#include "health.h"
#include "config.h"

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

#define RECOVERY_NS   "recovery"
#define RECOVERY_KEY  "last_recovery_reason"
#define REASON_STR    "soft_recovery_threshold"

/* Common fixture: pristine mocks, real LED driver on mocks, health
 * task wired (subscriptions + recovery-cb registration happen inside
 * health_task_start on the host too). */
static void fixture_reset(void)
{
    /* led_init is idempotent on a process-lifetime flag while the
     * timer mock reset wipes the handle table — deinit FIRST so
     * led_init re-creates FRESH handles (test_led_backoff_recovery
     * :37-39 idiom), then clear all mock state. */
    led_deinit();

    mock_esp_event_reset();
    mock_esp_timer_reset();
    mock_esp_system_reset();
    mock_nvs_reset();
    mock_log_reset();
    mock_gpio_reset();

    TEST_ASSERT_EQUAL(ESP_OK, led_init());
    TEST_ASSERT_EQUAL(ESP_OK, health_task_start());
}

/* One outage episode: DISCONNECTED (ingest) + GOT_IP (closes it),
 * spaced `step_s` apart on the primed µs clock. */
static int64_t s_now_us;
static void fire_outage_episode(void)
{
    s_now_us += 20LL * 1000000LL;
    mock_esp_timer_get_time_set_return(s_now_us);
    (void)mock_esp_event_fire_handler(WIFI_EVENT,
                                      WIFI_EVENT_STA_DISCONNECTED, NULL);
    mock_esp_timer_get_time_set_return(s_now_us + 1000000LL);
    (void)mock_esp_event_fire_handler(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
}

static bool read_back_reason(char *buf, size_t cap)
{
    nvs_handle_t h;
    if (mock_nvs_open(RECOVERY_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    esp_err_t err = mock_nvs_get_str(h, RECOVERY_KEY, buf, &len);
    mock_nvs_close(h);
    return err == ESP_OK;
}

/* ---------- S1 — strict sequence persist → LED-arm → cb → restart ---------- */
TEST_CASE(
    "test_soft_recovery_threshold_sequence_persist_before_restart [fw-16.1][trigger][scenario-S1]",
    "[health][fw-16.1][trigger]")
{
    fixture_reset();
    s_now_us = 0;

    /* 29 distinct episodes: below threshold, nothing may fire. */
    for (int i = 0; i < 29; ++i) {
        fire_outage_episode();
    }
    TEST_ASSERT_EQUAL_INT(0, mock_esp_restart_call_count());
    TEST_ASSERT_EQUAL_INT(0, mock_esp_timer_start_once_call_count());

    /* Episode #30 crosses the threshold INSIDE the handler. */
    s_now_us += 20LL * 1000000LL;
    mock_esp_timer_get_time_set_return(s_now_us);
    (void)mock_esp_event_fire_handler(WIFI_EVENT,
                                      WIFI_EVENT_STA_DISCONNECTED, NULL);

    /* ① persisted BEFORE any restart call is observed … */
    char buf[64] = {0};
    TEST_ASSERT_TRUE_MESSAGE(read_back_reason(buf, sizeof(buf)),
                             "reason not persisted before restart");
    TEST_ASSERT_EQUAL_STRING(REASON_STR, buf);
    /* … ② SOFT_RECOVERY armed the 3 s one-shot … */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, mock_esp_timer_start_once_call_count(),
                                  "LED one-shot not armed on trigger");
    /* … ③→④ have NOT happened yet: still running. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_esp_restart_call_count(),
                                  "restart observed before the "
                                  "recovery-complete callback fired");

    /* The one-shot fires the registered completion cb → restart. */
    esp_err_t fr = mock_esp_timer_fire_callback(mock_esp_timer_handle_at(1));
    TEST_ASSERT_EQUAL(ESP_OK, fr);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, mock_esp_restart_call_count(),
                                  "completion cb did not restart exactly "
                                  "once");
}

/* ---------- S2 — latched + idempotent after firing ---------- */
TEST_CASE(
    "test_soft_recovery_trigger_latched_idempotent_after_firing [fw-16.1][trigger][scenario-S2]",
    "[health][fw-16.1][trigger]")
{
    fixture_reset();
    s_now_us = 0;

    /* Cross the threshold and fire the completion cb. */
    for (int i = 0; i < 30; ++i) {
        fire_outage_episode();
    }
    s_now_us += 20LL * 1000000LL;
    mock_esp_timer_get_time_set_return(s_now_us);
    (void)mock_esp_event_fire_handler(WIFI_EVENT,
                                      WIFI_EVENT_STA_DISCONNECTED, NULL);
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_once_call_count());
    (void)mock_esp_timer_fire_callback(mock_esp_timer_handle_at(1));
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());

    /* Post-restart noise (paired bursts + reconnects): the trigger
     * latch holds — no second persist, no second LED arm, no second
     * restart. */
    int writes_after_trigger = mock_nvs_write_count();
    for (int i = 0; i < 5; ++i) {
        fire_outage_episode();
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, mock_esp_restart_call_count(),
                                  "trigger re-fired after restart");
    TEST_ASSERT_EQUAL_INT_MESSAGE(writes_after_trigger,
                                  mock_nvs_write_count(),
                                  "reason re-persisted after trigger");
    TEST_ASSERT_EQUAL_INT(1, mock_esp_timer_start_once_call_count());
}

/* ---------- G1 — 60 s healthy connected streaming never triggers ---------- */
TEST_CASE(
    "test_soft_recovery_healthy_stream_60s_never_triggers [fw-16.3][green-path][scenario-G1]",
    "[health][fw-16.3][green-path]")
{
    fixture_reset();

    /* Healthy connected streaming: GOT_IP-state heartbeats every
     * second for 60 simulated seconds. Evaluations are driven ONLY
     * through the existing event entry points — production has no
     * periodic sweep (R-FW16-1.3). */
    mock_esp_timer_get_time_set_return(0);
    (void)mock_esp_event_fire_handler(IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    for (int sec = 1; sec <= 60; ++sec) {
        mock_esp_timer_get_time_set_return((int64_t)sec * 1000000LL);
        (void)mock_esp_event_fire_handler(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          NULL);
    }

    /* Counter untouched ⇒ zero NVS writes, LED never armed, no
     * restart. A future always-sweeping miscounting implementation
     * breaks this (see Pass 13 bite-proof). */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_nvs_write_count(),
                                  "healthy-stream invariant violated: "
                                  "recovery artifacts written during "
                                  "healthy streaming");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_esp_timer_start_once_call_count(),
                                  "healthy-stream invariant violated: "
                                  "SOFT_RECOVERY armed during healthy "
                                  "streaming");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_esp_restart_call_count(),
                                  "healthy-stream invariant violated: "
                                  "restart during healthy streaming");
}
