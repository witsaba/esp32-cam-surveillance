/* esp_netif.h — host stub of IDF's esp_netif.h (FW-05 host tests).
 *
 * The production source calls esp_netif_create_default_wifi_ap and
 * esp_netif_destroy_default_netif. Both are redirected to mocks via
 * mock_esp_netif_link.h BEFORE this header is included.
 *
 * mock_esp_netif.h uses `typedef void *esp_netif_handle_t;` which is
 * compatible with the typedef below (declared once in this header).
 * We do NOT re-declare the function prototypes — the link-header
 * redirect would conflict with the mock's real prototypes.
 */
#ifndef HOST_ESP_NETIF_H
#define HOST_ESP_NETIF_H

#include "esp_err.h"

typedef void *esp_netif_handle_t;

#endif /* HOST_ESP_NETIF_H */
