/* mock_esp_wifi_link.h — macro-redirect for the Wi-Fi API.
 *
 * Mirrors mock_nvs_flash_link.h: every esp_wifi_* call site is
 * replaced by the mock_* symbol below when MOCK_WIFI_USE_REAL is
 * NOT defined. Production source includes this BEFORE <esp_wifi.h>.
 *
 * On host (UNITY_HOST_BUILD defined), the macros are active. On
 * device, the macros are inactive and the real esp_wifi_* symbols
 * from the IDF Wi-Fi component are linked.
 */
#pragma once

#include "mock_esp_wifi.h"

#ifndef MOCK_WIFI_USE_REAL

#define esp_wifi_set_mode(mode)              mock_esp_wifi_set_mode(mode)
#define esp_wifi_set_config(iface, cfg)      mock_esp_wifi_set_config(iface, cfg)
#define esp_wifi_start()                     mock_esp_wifi_start()
#define esp_wifi_stop()                      mock_esp_wifi_stop()

#endif  /* !MOCK_WIFI_USE_REAL */
