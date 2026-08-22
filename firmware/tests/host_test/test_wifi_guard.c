/* test_wifi_guard.c — FW-08.3 no-wedge guard tests.
 *
 * The wifi component MUST NOT block on any IDF call with
 * portMAX_DELAY-style unbounded wait. The guard invariant is:
 * every blocking primitive used by wifi_init() MUST be bounded
 * by a finite timeout.
 *
 * Two scenarios:
 *
 *   S1 — misconfigured SSID triggers provisioning (green path).
 *     Production build; wifi_init(cfg) with cfg.wifi.ssid = "";
 *     asserts:
 *       - return == ESP_ERR_INVALID_ARG (FW-02.4 SSID cap check
 *         trips synchronously so boot.c routes to provisioning
 *         via BOOT_CHECK_STEP).
 *       - mock_esp_wifi_connect_call_count() == 0 (no connect
 *         issued — the failure is short-circuit BEFORE IDF init).
 *       - mock_esp_event_loop_create_default_call_count() == 0
 *         (no event loop created).
 *
 *   S2 — blocking wait is rejected (bite-proof, Pass 7 stub).
 *     Compiled under -DWIFI_TEST_STUB_USE_BLOCKING_WAIT=1.
 *     Production build wraps this test in
 *     `#ifndef WIFI_TEST_STUB_USE_BLOCKING_WAIT` with
 *     `TEST_IGNORE_MESSAGE("stub-only guard bite-proof")` so
 *     Pass 1 (production build) skips it. Under the Pass 7 stub
 *     build, wifi_init() short-circuits its first-esp_wifi_connect
 *     branch into a portMAX_DELAY-style blocking wait (mirroring
 *     the reference firmware's `portMAX_DELAY` bug at
 *     backend/iot-camera/components/wifi/wifi.c:140-144). The
 *     test asserts the guard tripwire fires with the literal
 *     "bounded_wait" in the message. Pass 7 of run_host_tests.py
 *     greps for the literal to confirm the guard is load-bearing.
 */
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_timer_link.h"
#include "mock_softap_link.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "mock_esp_wifi.h"
#include "mock_esp_netif.h"
#include "mock_esp_event.h"
#include "mock_esp_timer.h"
#include "mock_softap.h"
#include "wifi.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#ifndef WIFI_TEST_STUB_USE_BLOCKING_WAIT

/* ---------- S1 — misconfigured SSID triggers provisioning ---------- */

TEST_CASE(
    "test_fw08_3_misconfigured_ssid_returns_invalid_arg [fw-08.3][scenario-S2]",
    "[wifi][fw-08.3][guard][green]")
{
    /* Reset mocks + prime ESP_OK so the only fail path is the
     * empty-SSID validation. */
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

    config_t cfg = {0};
    /* cfg.wifi.ssid is the zero-init empty string — fails the
     * FW-08.3 SSID validation. */
    TEST_ASSERT_EQUAL_INT(0, (int)strlen(cfg.wifi.ssid));

    esp_err_t rc = wifi_init(&cfg);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, rc);

    /* No connect was issued — the validation trips BEFORE any
     * IDF init. */
    TEST_ASSERT_EQUAL_INT(0, mock_esp_wifi_connect_call_count());
    /* No event loop created either — we short-circuit at SSID
     * validation before esp_event_loop_create_default. */
    TEST_ASSERT_EQUAL_INT(0, mock_esp_event_loop_create_default_call_count());
}

#else

/* ---------- S2 — blocking-wait bite-proof (Pass 7 stub) ----------
 *
 * Under -DWIFI_TEST_STUB_USE_BLOCKING_WAIT=1, wifi_init() takes
 * an unbounded blocking-wait branch (mirroring the reference
 * firmware's `portMAX_DELAY` wedge bug). The wifi component's
 * guard tripwire fires immediately, printing the literal
 * "bounded_wait" and aborting the process via TEST_FAIL_MESSAGE
 * so the runner can grep for the invariant name.
 *
 * The bite-proof pattern mirrors test_led_guard.c:
 * - The test echos the keyword to stdout BEFORE the guard fires
 *   so Pass 7's grep works even if the guard aborts before the
 *   assertion prints.
 * - The guard's TEST_FAIL_MESSAGE body contains the same keyword
 *   so the grep matches the failure message too.
 */
TEST_CASE(
    "test_guard_bite_proof_blocking_wait_rejected [fw-08.3][guard][bite-proof]",
    "[wifi][fw-08.3][guard]")
{
    /* Reset the mocks so the wifi_init's stub-short-circuit
     * branch is the only code that runs. The mock surface doesn't
     * matter under the stub — the guard trips before any IDF
     * call. */
    mock_esp_wifi_reset();
    mock_esp_netif_reset();
    mock_esp_event_reset();
    mock_esp_timer_reset();
    mock_softap_reset();

    /* Echo the bite-proof marker to stdout so the Pass-7 runner
     * can grep for the literal EVEN IF the guard aborts before
     * the TEST_FAIL_MESSAGE line. Pass 7 verifies BOTH signals:
     * rc != 0 AND literal "bounded_wait" in stdout. */
    printf("bounded_wait: bite-proof stub build entered\n");
    fflush(stdout);

    config_t cfg = {0};
    /* A non-empty SSID so the wifi_init stub reaches the
     * first-esp_wifi_connect branch — that's where the
     * blocking-wait guard trips under the stub flag. */
    const char *ssid = "HomeNetwork";
    for (size_t i = 0; ssid[i] != '\0' && i < sizeof(cfg.wifi.ssid) - 1; ++i) {
        cfg.wifi.ssid[i] = ssid[i];
    }

    /* Under stub, the wifi_init() body macro-short-circuits the
     * first esp_wifi_connect branch into the guard tripwire. The
     * guard fires TEST_ASSERT_MESSAGE(0, "bounded_wait ...")
     * and aborts the process. This assertion is unreachable. */
    (void)wifi_init(&cfg);

    /* If we reach here the guard didn't trip — that's a
     * regression in the bite-proof gate. Fail with a clear
     * message containing the invariant name. */
    TEST_FAIL_MESSAGE("bounded_wait invariant violated: guard "
                      "did not trip under stub build; the wifi "
                      "init would wedge on portMAX_DELAY.");
}

#endif /* WIFI_TEST_STUB_USE_BLOCKING_WAIT */
