/* mock_esp_event.h — host-side mock for the IDF event loop driver.
 *
 * FW-05 softAP bring-up calls esp_event_loop_create_default() before
 * esp_netif_create_default_wifi_ap() and esp_wifi_init() — IDF v5.5.3
 * requires the default event loop to exist before the wifi driver can
 * deliver WIFI_EVENT_AP_START and friends (caught on device flash,
 * engram #3630). On host the mock returns whatever the test primed
 * via set_*_return_set() (default ESP_OK); the production source
 * already treats ESP_ERR_INVALID_STATE as success (idempotent init).
 */
#pragma once

#include "esp_err.h"

/* ---------- primable return values ---------- */
void mock_esp_event_loop_create_default_return_set(esp_err_t r);

/* ---------- call counters ---------- */
int  mock_esp_event_loop_create_default_call_count(void);

/* ---------- reset ---------- */
void mock_esp_event_reset(void);

/* ---------- mock target ---------- */
esp_err_t mock_esp_event_loop_create_default(void);
