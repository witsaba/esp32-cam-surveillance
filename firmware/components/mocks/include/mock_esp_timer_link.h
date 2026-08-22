/* mock_esp_timer_link.h — macro-redirect for the esp_timer API.
 *
 * Mirrors mock_esp_wifi_link.h: every esp_timer_* call site is
 * replaced by the mock_* symbol below when
 * MOCK_ESP_TIMER_USE_REAL is NOT defined. Production source
 * (led.c) includes this BEFORE <esp_timer.h>.
 *
 * On host (UNITY_HOST_BUILD defined), the macros are active.
 * On device, the macros are inactive and the real esp_timer_*
 * symbols from the IDF esp_timer component are linked.
 */
#pragma once

#include "mock_esp_timer.h"

#ifndef MOCK_ESP_TIMER_USE_REAL

#define esp_timer_create(args, out)             mock_esp_timer_create(args, out)
#define esp_timer_start_periodic(h, us)         mock_esp_timer_start_periodic(h, us)
#define esp_timer_start_once(h, us)             mock_esp_timer_start_once(h, us)
#define esp_timer_stop(h)                       mock_esp_timer_stop(h)
#define esp_timer_restart(h, us)                mock_esp_timer_restart(h, us)
#define esp_timer_delete(h)                     mock_esp_timer_delete(h)

#endif  /* !MOCK_ESP_TIMER_USE_REAL */
