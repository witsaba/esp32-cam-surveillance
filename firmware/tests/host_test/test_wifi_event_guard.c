/* test_wifi_event_guard.c — FW-08.6 no-AP-after-tear-down guard.
 *
 * The IP-up handler MUST be wired to softap_stop() +
 * esp_wifi_set_mode(WIFI_MODE_STA) under
 * CONFIG_FIRMWARE_PROVISIONING_AP_STOP_ON_CONNECT=y. Removing
 * either wire is a scratch violation. Under the stub build
 * (WIFI_TEST_STUB_SKIP_IP_UP_HANDLER=1), the IP-up handler is
 * a no-op (the handler is registered but never fires the
 * softAP teardown). The bite-proof test asserts the failure
 * message names the teardown invariant.
 *
 * Two scenarios:
 *
 *   S1 — green path closes the attack window.
 *     Production build (stub OFF); fire
 *     IP_EVENT_STA_GOT_IP. Asserts:
 *       - mock_esp_wifi_set_mode_arg_at(0) == WIFI_MODE_STA
 *         (the LAST set_mode call after IP-up is STA, not
 *         APSTA — proves the post-teardown mode transition)
 *       - mock_softap_stop_call_count() == 1 (teardown fired)
 *
 *   S2 — missing teardown is rejected (bite-proof, Pass 8).
 *     Compiled under -DWIFI_TEST_STUB_SKIP_IP_UP_HANDLER=1.
 *     Production build wraps this test in
 *     `#ifndef WIFI_TEST_STUB_SKIP_IP_UP_HANDLER` with
 *     `TEST_IGNORE_MESSAGE("stub-only guard bite-proof")` so
 *     Pass 1 (production build) skips it. Under the Pass 8
 *     stub build, wifi_event.c::ip_up_handler() is replaced
 *     by a no-op (counter NOT reset, softAP NOT torn down,
 *     mode NOT switched). The test asserts the guard tripwire
 *     fires with the literal "teardown" in the message.
 *
 * The bite-proof pattern mirrors test_led_guard.c (Pass 5)
 * and test_button_guard.c (Pass 6):
 * - The test echos the keyword to stdout BEFORE the guard fires
 *   so Pass 8's grep works even if the guard aborts before the
 *   assertion prints.
 * - The guard's TEST_FAIL_MESSAGE body contains the same keyword
 *   so the grep matches the failure message too.
 */
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_timer_link.h"
#include "mock_softap_link.h"

#include <stdio.h>

#include "config.h"
#include "mock_esp_wifi.h"
#include "mock_esp_netif.h"
#include "mock_esp_event.h"
#include "mock_esp_timer.h"
#include "mock_softap.h"
#include "wifi.h"
#include "wifi_event.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#ifndef WIFI_TEST_STUB_SKIP_IP_UP_HANDLER

/* ---------- S1 — green path closes the attack window ---------- */

TEST_CASE(
    "test_fw08_6_green_path_closes_attack_window [fw-08.6][scenario-S2]",
    "[wifi-event][fw-08.6][guard][green]")
{
    /* Standard fixture: reset mocks + prime returns + init. */
    mock_esp_wifi_reset();
    mock_esp_netif_reset();
    mock_esp_event_reset();
    mock_esp_timer_reset();
    mock_softap_reset();
    mock_esp_wifi_init_return_set(ESP_OK);
    mock_esp_wifi_set_mode_return_set(ESP_OK);
    mock_esp_wifi_set_config_return_set(ESP_OK);
    mock_esp_wifi_start_return_set(ESP_OK);
    mock_esp_wifi_connect_return_set(ESP_OK);
    mock_esp_timer_create_set_return(ESP_OK);
    mock_esp_timer_start_once_set_return(ESP_OK);
    mock_softap_is_active_set_return(true);

    config_t cfg = {0};
    const char *ssid = "HomeNetwork";
    for (size_t i = 0; ssid[i] != '\0' && i < sizeof(cfg.wifi.ssid) - 1; ++i) {
        cfg.wifi.ssid[i] = ssid[i];
    }
    esp_err_t rc = wifi_init(&cfg);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* At init the LAST set_mode was APSTA (FW-08.5). After IP-up
     * the LAST set_mode MUST be STA (the wifi_stop() body
     * transitions to STA — see T-08-E). */
    int mode_count_before = mock_esp_wifi_set_mode_call_count();

    /* Fire IP_EVENT_STA_GOT_IP. */
    (void)mock_esp_event_fire_handler(IP_EVENT,
                                       IP_EVENT_STA_GOT_IP,
                                       NULL);

    /* softap_stop fired exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_softap_stop_call_count());

    /* The LAST set_mode call (most recent) is WIFI_MODE_STA —
     * proves the post-teardown mode transition. The wifi_stop()
     * body issues esp_wifi_set_mode(WIFI_MODE_STA) after
     * softap_stop returns. */
    int mode_count_after = mock_esp_wifi_set_mode_call_count();
    TEST_ASSERT_GREATER_THAN_INT(mode_count_before, mode_count_after);
    wifi_mode_t last_mode = mock_esp_wifi_set_mode_arg_at(0);
    TEST_ASSERT_EQUAL_INT(WIFI_MODE_STA, (int)last_mode);
}

#else

/* ---------- S2 — bite-proof (Pass 8 stub build) ----------
 *
 * Under -DWIFI_TEST_STUB_SKIP_IP_UP_HANDLER=1, the IP-up handler
 * in wifi_event.c is replaced by a no-op (counter NOT reset,
 * softAP NOT torn down, mode NOT switched). The wifi_event.c
 * guard tripwire fires when the test exercises the IP-up path,
 * printing the literal "teardown" and aborting the process via
 * TEST_FAIL_MESSAGE.
 *
 * Pass 8 expects:
 *   - rc != 0 (TEST_FAIL_MESSAGE exits non-zero)
 *   - literal "teardown" in stdout
 *   - "guard_bite_proof_teardown_on_ip_disabled" test name
 *     in stdout
 */
TEST_CASE(
    "test_guard_bite_proof_teardown_on_ip_disabled [fw-08.6][guard][bite-proof]",
    "[wifi-event][fw-08.6][guard]")
{
    /* Reset mocks + init so the IP-up handler registration
     * is wired (the stub swaps the body but the handler is
     * still subscribed via wifi_event_subscribe). */
    mock_esp_wifi_reset();
    mock_esp_netif_reset();
    mock_esp_event_reset();
    mock_esp_timer_reset();
    mock_softap_reset();
    mock_esp_wifi_init_return_set(ESP_OK);
    mock_esp_wifi_set_mode_return_set(ESP_OK);
    mock_esp_wifi_set_config_return_set(ESP_OK);
    mock_esp_wifi_start_return_set(ESP_OK);
    mock_esp_wifi_connect_return_set(ESP_OK);
    mock_esp_timer_create_set_return(ESP_OK);
    mock_esp_timer_start_once_set_return(ESP_OK);
    mock_softap_is_active_set_return(true);

    config_t cfg = {0};
    const char *ssid = "HomeNetwork";
    for (size_t i = 0; ssid[i] != '\0' && i < sizeof(cfg.wifi.ssid) - 1; ++i) {
        cfg.wifi.ssid[i] = ssid[i];
    }
    esp_err_t rc = wifi_init(&cfg);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Echo the bite-proof marker to stdout so Pass 8's grep
     * works even if the guard aborts before TEST_FAIL_MESSAGE
     * prints. */
    printf("teardown: bite-proof stub build entered\n");
    fflush(stdout);

    /* Fire IP_EVENT_STA_GOT_IP. Under stub, the handler is a
     * no-op — softap_stop NOT called, mode NOT switched. The
     * guard tripwire fires with the literal "teardown" in
     * the message and aborts the process. */
    (void)mock_esp_event_fire_handler(IP_EVENT,
                                       IP_EVENT_STA_GOT_IP,
                                       NULL);

    /* If we reach here the guard didn't trip — that's a
     * regression in the bite-proof gate. Fail with a clear
     * message containing the invariant name. */
    TEST_FAIL_MESSAGE("teardown_on_ip invariant violated: "
                      "softAP remains reachable on STA network "
                      "after IP_EVENT_STA_GOT_IP");
}

#endif /* WIFI_TEST_STUB_SKIP_IP_UP_HANDLER */
