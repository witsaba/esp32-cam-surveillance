/* mock_softap.c — implementation of the softap component mock.
 *
 * Mirrors mock_esp_wifi.c shape. Tests prime the softap_stop()
 * return + the softap_is_active() getter via the set_return
 * helpers. The wifi component (FW-08.4 + FW-08.5) consumes both
 * via the mock_softap link-header redirect.
 */
#include "mock_softap.h"

/* The stop contract mirror calls into the wifi mock (APSTA -> STA
 * mode switch) — see mock_softap_stop() below. */
#include "mock_esp_wifi.h"

static esp_err_t g_stop_return   = ESP_OK;
static bool      g_is_active     = false;

static int g_stop_count         = 0;

void mock_softap_stop_return_set(esp_err_t r) { g_stop_return = r; }
void mock_softap_is_active_set_return(bool active) { g_is_active = active; }

int mock_softap_stop_call_count(void) { return g_stop_count; }

void mock_softap_reset(void)
{
    g_stop_return = ESP_OK;
    g_is_active   = false;
    g_stop_count  = 0;
}

esp_err_t mock_softap_stop(void)
{
    g_stop_count++;
    /* Mirror the production contract (2026-08-24 GOT_IP-teardown
     * fix): softap_stop() switches APSTA -> STA instead of calling
     * esp_wifi_stop(), so the freshly-associated STA keeps its IP.
     * Modeling the mode switch here keeps the wifi-event suites'
     * "LAST set_mode == WIFI_MODE_STA" assertions load-bearing. */
    (void)mock_esp_wifi_set_mode(WIFI_MODE_STA);
    return g_stop_return;
}

bool mock_softap_is_active_get(void)
{
    return g_is_active;
}
