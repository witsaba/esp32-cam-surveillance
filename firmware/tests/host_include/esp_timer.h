/* esp_timer.h — host stub of IDF's esp_timer.h (FW-06 host tests).
 *
 * On host, the production source (led.c) calls esp_timer_create,
 * esp_timer_start_periodic, esp_timer_start_once, esp_timer_stop,
 * esp_timer_restart, esp_timer_delete. Each call site is
 * redirected to the mock via mock_esp_timer_link.h BEFORE this
 * header is included.
 *
 * The typedefs (esp_timer_handle_t, esp_timer_cb_t,
 * esp_timer_create_args_t) and constants (ESP_TIMER_TASK) live in
 * mock_esp_timer.h and are visible when this header is included
 * AFTER mock_esp_timer_link.h.
 */
#ifndef HOST_ESP_TIMER_H
#define HOST_ESP_TIMER_H

/* Pull in the mock types via the link-header redirect target. */
#include "mock_esp_timer.h"

#endif /* HOST_ESP_TIMER_H */
