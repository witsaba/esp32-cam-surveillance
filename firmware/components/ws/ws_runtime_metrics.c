/* ws_runtime_metrics.c — runtime-metrics collector skeleton
 * (FW-13, T-13-C GREEN-only).
 *
 * T-13-C scope: zero-fill the struct. The real getter-fanout
 * (esp_timer_get_time + esp_wifi_sta_get_rssi + esp_get_free
 * _heap_size + capture_fb_drops_get + ws_reconnects_get) lands
 * in T-13-I GREEN.
 */
#include "ws.h"
#include "ws_reconnects.h"

#include <string.h>

#ifdef UNITY_HOST_BUILD
#include "mock_esp_timer_link.h"
#include "mock_esp_wifi_link.h"
#include "mock_esp_system_link.h"
#else
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h>
#endif

#include "capture.h"

void ws_runtime_metrics_collect(ws_runtime_metrics_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* T-13-I: real getter-fanout lands here. */
}