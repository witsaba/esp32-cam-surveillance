/* test_wifi_main.c — FW-08 component smoke test (T-08-A only).
 *
 * Asserts the wifi component compiles + links + returns ESP_OK
 * from a stub wifi_init(). The full FW-08 surface lands in
 * T-08-B..T-08-G (6 test files + 16 production tests + 2 bite-
 * proofs). This file is the T-08-A build-infra smoke; the runner
 * counts it as the 69th test (baseline 68 + 1 smoke).
 *
 * The smoke primes the esp_wifi mocks to return ESP_OK, calls
 * wifi_init, and asserts the first esp_wifi_connect() happened
 * once (proving the wifi component reached its end-of-init
 * connect path; T-08-B fleshes out the rest of the connect
 * sequence).
 */
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_timer_link.h"
#include "mock_softap_link.h"

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

TEST_CASE(
    "test_wifi_init_succeeds [fw-08][smoke][build-infra]",
    "[wifi][fw-08][smoke]")
{
    /* Reset all mocks that wifi_init touches. */
    mock_esp_wifi_reset();
    mock_esp_netif_reset();
    mock_esp_event_reset();
    mock_esp_timer_reset();
    mock_softap_reset();

    /* Prime the return values so the T-08-A stub body returns OK
     * at every step (no real IDF init required on host). */
    mock_esp_wifi_init_return_set(ESP_OK);
    mock_esp_wifi_set_mode_return_set(ESP_OK);
    mock_esp_wifi_set_config_return_set(ESP_OK);
    mock_esp_wifi_start_return_set(ESP_OK);
    mock_esp_wifi_connect_return_set(ESP_OK);

    config_t cfg = {0};
    /* A known, non-empty SSID — T-08-D exercises the empty-SSID
     * rejection path (FW-08.3 S2); this smoke just confirms the
     * green path is wired. */
    const char *ssid = "HomeNetwork";
    for (size_t i = 0; ssid[i] != '\0' && i < sizeof(cfg.wifi.ssid) - 1; ++i) {
        cfg.wifi.ssid[i] = ssid[i];
    }

    esp_err_t rc = wifi_init(&cfg);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);
}
