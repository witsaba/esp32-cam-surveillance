/* esp_event.h — host stub of IDF's esp_event.h (FW-05 host tests).
 *
 * The production source (softap.c) calls esp_event_loop_create_default()
 * after esp_netif_init() and before esp_wifi_init(). Each call site is
 * redirected to the mock via mock_esp_event_link.h BEFORE this header
 * is included. The mock implements the function; we do NOT re-declare
 * prototypes here — the link-header redirect would turn them into
 * conflicting mock_* prototypes.
 *
 * Constants / typedefs that aren't function calls live in IDF's
 * real esp_event.h; we don't need any here (no enum or struct is
 * dereferenced by the production source on this path).
 */
#ifndef HOST_ESP_EVENT_H
#define HOST_ESP_EVENT_H

#include "esp_err.h"

#endif /* HOST_ESP_EVENT_H */
