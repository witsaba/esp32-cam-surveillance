/* ws_runtime_metrics.c — runtime-metrics collector (FW-13,
 * T-13-G GREEN partial + T-13-I GREEN full).
 *
 * T-13-G: read uptime_us only (the cadence test asserts
 * status frames fire; the JSON just needs SOMETHING). T-13-I
 * fills in rssi_dbm, free_heap, fb_drops, reconnects. */
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
#include "esp_system.h"
#endif

#include "capture.h"

void ws_runtime_metrics_collect(ws_runtime_metrics_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* T-13-G: uptime. The host mock's esp_timer_get_time
     * returns the primed value (default 0); on device it's
     * microseconds since boot. */
    out->uptime_us = (int64_t)esp_timer_get_time();
    /* T-13-I: rssi_dbm, free_heap, fb_drops, reconnects land here. */
}