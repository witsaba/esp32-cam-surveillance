/* esp_mac.h — host stub of IDF's esp_mac.h.
 *
 * The IDF real esp_mac.h is at $IDF_PATH/components/esp_hw_support/include/
 * but its inclusion chain pulls in many device-only headers. On host we
 * only need the constant ESP_MAC_WIFI_STA (used by softap_handlers.c) —
 * the mock_esp_system.h provides ESP_MAC_WIFI_STA but does not redefine
 * the production symbol. We keep the include order: mock_esp_system.h
 * defines ESP_MAC_WIFI_STA before this header is read by the production
 * source. */
#ifndef HOST_ESP_MAC_H
#define HOST_ESP_MAC_H

/* Nothing to declare — ESP_MAC_WIFI_STA / ESP_MAC_WIFI_AP / ESP_MAC_BT /
 * ESP_MAC_ETH come from mock_esp_system.h which is included earlier
 * in the production source. */

#endif /* HOST_ESP_MAC_H */
