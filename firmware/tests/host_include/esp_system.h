/* esp_system.h — host stub of IDF's esp_system.h (FW-05 host tests).
 *
 * The production source (softap_handlers.c) calls esp_read_mac,
 * esp_chip_info, esp_get_idf_version, esp_restart. Each is redirected
 * to the mock via mock_esp_system_link.h BEFORE this header is
 * included.
 *
 * The chip-info / mac-type typedefs and function prototypes live in
 * mock_esp_system.h and are visible when this header is included
 * AFTER mock_esp_system_link.h. We do NOT re-declare prototypes here
 * — the link-header redirect would conflict with the mock's.
 */
#ifndef HOST_ESP_SYSTEM_H
#define HOST_ESP_SYSTEM_H

#include "esp_err.h"
#include <stdint.h>

#endif /* HOST_ESP_SYSTEM_H */
