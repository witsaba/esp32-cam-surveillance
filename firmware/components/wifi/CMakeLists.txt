/* CMakeLists.txt — wifi component (FW-08).
 *
 * Single component, two TUs (wifi.c connect driver + wifi_event.c
 * handlers). Mirrors the FW-06 led.c single-component convention:
 * keeping the consecutive_failures counter module-static in
 * wifi_event.c avoids a cross-TU header burden.
 *
 * REQUIRES esp_wifi esp_netif esp_event esp_timer led softap config
 * — every IDF API the component calls is in one of those surface
 * mocks + the led driver for state transitions + softap for the
 * IP-up teardown + config for cfg.wifi.ssid validation.
 */
idf_component_register(
    SRCS "wifi.c"
         "wifi_event.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer led softap config
)
