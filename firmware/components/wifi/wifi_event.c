/* wifi_event.c — Wi-Fi event handlers + counter (FW-08).
 *
 * Owned by the wifi component (single component, two TUs).
 * Counter is module-static; reset on IP_EVENT_STA_GOT_IP per
 * explore #3681 § Findings §10 + proposal #3682 § Decision §5.
 *
 * Stub body for T-08-A: file is empty for now. T-08-C introduces
 * the IP-up handler + counter + handler-registration plumbing;
 * T-08-E adds the softap teardown branch; T-08-G adds the FW-08.6
 * teardown guard.
 */
#include "wifi_event.h"
#include "wifi.h"

#include "esp_err.h"
#include "led.h"
#include "softap.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_wifi_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_timer_link.h"
#else
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#endif

/* Intentionally no symbols yet — the wifi component compiles
 * + links with this empty TU. T-08-C introduces:
 *   - static uint32_t s_consecutive_failures = 0;
 *   - on_sta_disconnected_handler(void *arg, ...)
 *   - on_sta_got_ip_handler(void *arg, ...)
 *   - esp_timer_cb_t s_retry_cb
 *   - wifi_event_register() helper
 */
