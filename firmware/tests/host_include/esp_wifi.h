/* esp_wifi.h — host stub of IDF's esp_wifi.h (FW-05 host tests).
 *
 * On host, the production source (softap.c) calls esp_wifi_set_mode,
 * esp_wifi_set_config, esp_wifi_ap_start, and esp_wifi_stop. Each
 * call site is redirected to the mock via mock_esp_wifi_link.h BEFORE
 * this header is included. The mock implements the function; we do
 * NOT re-declare prototypes here — the link-header redirect would
 * turn them into conflicting mock_* prototypes.
 *
 * The constants WIFI_MODE_AP / WIFI_IF_AP live here (they're not
 * function calls and don't redirect). The typedefs (wifi_mode_t,
 * wifi_interface_t, wifi_config_t) live in mock_esp_wifi.h and are
 * visible when this header is included AFTER mock_esp_wifi_link.h.
 */
#ifndef HOST_ESP_WIFI_H
#define HOST_ESP_WIFI_H

#include "esp_err.h"

/* Constants matching IDF v5.5.3. */
#define WIFI_MODE_AP     2
#define WIFI_IF_AP       1

#endif /* HOST_ESP_WIFI_H */
