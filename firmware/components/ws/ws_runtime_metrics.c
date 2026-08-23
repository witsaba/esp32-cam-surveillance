/* ws_runtime_metrics.c — runtime-metrics collector (FW-13,
 * T-13-H GREEN).
 *
 * Reads the documented 5-field runtime snapshot:
 *   - uptime_us   — esp_timer_get_time()
 *   - rssi_dbm    — esp_wifi_sta_get_rssi() (mock-friendly)
 *   - free_heap   — esp_get_free_heap_size()
 *   - fb_drops    — capture_fb_drops_get() (FW-11.2 counter)
 *   - reconnects  — ws_reconnects_get() (FW-14 owner; returns 0
 *                   in FW-13)
 *
 * Zero-initialises the struct first so partial fills leave the
 * struct in a known state. The status-frame builder (T-13-H
 * GREEN) reads the populated struct directly. */
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

    /* uptime_us — microseconds since boot (host mock returns the
     * primed value, default 0). */
    out->uptime_us = (int64_t)esp_timer_get_time();

    /* rssi_dbm — station RSSI in dBm. On host the mock returns
     * the primed value (default -50 dBm per mock_esp_wifi.c);
     * on device it queries the wifi driver. */
    int32_t rssi = 0;
    if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
        out->rssi_dbm = rssi;
    }

    /* free_heap — esp_get_free_heap_size() (mock default 200 KB). */
    out->free_heap = esp_get_free_heap_size();

    /* fb_drops — frame-buffer drops counter from the FW-11
     * capture task. Returns 0 if capture is not running. */
    out->fb_drops = capture_fb_drops_get();

    /* reconnects — FW-14 owns the producer; the stub returns 0
     * in FW-13. The symbol resolution picks the production impl
     * (ws_reconnects.c) on device. */
    out->reconnects = ws_reconnects_get();
}
