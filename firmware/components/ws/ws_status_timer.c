/* ws_status_timer.c — periodic 30 s status-timer implementation
 * (FW-13, T-13-G GREEN; FW-16 server-mode rewiring).
 *
 * The timer fires every CONFIG_FIRMWARE_WS_STATUS_PERIOD_MS ms
 * (default 30000 = 30 s). The callback reads runtime metrics +
 * identity, builds the JSON, and pushes the status frame through
 * the viewer sink (httpd_ws_send_frame_async on device).
 *
 * Lifecycle:
 *   ws_status_timer_init()   — create the periodic handle (once)
 *   ws_status_timer_start()  — arm on /cams viewer accept
 *   ws_status_timer_stop()   — disarm on viewer socket close
 *
 * The module-static handle is exposed via
 * ws_status_timer_handle_get() so host tests can advance it
 * via mock_esp_timer_advance_periodic().
 */
#include "ws.h"
#include "identity.h"
#include "ws_reconnects.h"

#include <string.h>

#ifdef UNITY_HOST_BUILD
#include "mock_esp_websocket_client_link.h"
#include "mock_esp_timer_link.h"
#else
#include "esp_websocket_client.h"
#include "esp_timer.h"
#endif

#include "esp_log.h"

static const char *TAG = "ws_status";

/* Module-static handle + period. The handle is created in
 * ws_status_timer_init() and reused for start/stop cycles
 * (matches the FW-06 LED status-timer pattern). */
static esp_timer_handle_t g_status_timer = NULL;
static uint64_t g_status_period_us = 0;

/* Status-builder buffer (sized for the documented 8-field JSON
 * schema; 512 bytes covers the worst case). */
#define WS_STATUS_BUF_LEN 512

/* The periodic callback. Reads runtime metrics + identity,
 * builds the JSON, sends as a text frame through the viewer
 * sink. Silently skips (after one WARN) when no viewer is
 * connected — the timer is stopped on socket close anyway; this
 * guards the race where the callback fires between close and
 * stop.
 *
 * The host mock fires the callback synchronously via
 * mock_esp_timer_fire_callback or the advance_periodic helper,
 * so the production send path runs deterministically without
 * FreeRTOS timers. */
static void ws_status_timer_cb(void *arg)
{
    (void)arg;
    if (!ws_sink_connected()) {
        ESP_LOGW(TAG, "status timer fired without an active viewer");
        return;
    }

    /* Read runtime metrics (uptime, rssi, heap, fb_drops,
     * reconnects). */
    ws_runtime_metrics_t metrics;
    ws_runtime_metrics_collect(&metrics);

    /* Re-load identity for the latest name (NVS could have
     * changed via /provision). MAC is the canonical eFuse read
     * so it always matches; name/description may have been
     * updated by the operator. */
    device_identity_t id;
    esp_err_t r = identity_load(&id);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "status cb: identity_load failed: %s",
                 esp_err_to_name(r));
        return;
    }

    char buf[WS_STATUS_BUF_LEN];
    size_t len = ws_text_frame_build_status(&metrics, &id,
                                             buf, sizeof(buf));
    if (len == 0) {
        ESP_LOGE(TAG, "status cb: ws_text_frame_build_status "
                       "returned 0; skipping send");
        return;
    }
    esp_err_t sr = ws_sink_send_text(buf, len);
    if (sr != ESP_OK) {
        ESP_LOGE(TAG, "status cb: send failed: %s", esp_err_to_name(sr));
    } else {
        ESP_LOGD(TAG, "status sent: %u bytes (uptime=%lld s "
                       "rssi=%d heap=%u fb_drops=%u reconnects=%u)",
                 (unsigned)len,
                 (long long)(metrics.uptime_us / 1000000),
                 (int)metrics.rssi_dbm,
                 (unsigned)metrics.free_heap,
                 (unsigned)metrics.fb_drops,
                 (unsigned)metrics.reconnects);
    }
}

esp_err_t ws_status_timer_init(void)
{
    if (g_status_timer != NULL) return ESP_OK;

    g_status_period_us =
        (uint64_t)CONFIG_FIRMWARE_WS_STATUS_PERIOD_MS * 1000ULL;

    esp_timer_create_args_t args = {
        .callback = ws_status_timer_cb,
        .arg      = NULL,
        .name     = "ws_status",
    };
    esp_err_t r = esp_timer_create(&args, &g_status_timer);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s",
                 esp_err_to_name(r));
        g_status_timer = NULL;
        return r;
    }
    return ESP_OK;
}

esp_err_t ws_status_timer_start(void)
{
    if (g_status_timer == NULL) {
        esp_err_t r = ws_status_timer_init();
        if (r != ESP_OK) return r;
    }
    return esp_timer_start_periodic(g_status_timer,
                                       g_status_period_us);
}

esp_err_t ws_status_timer_stop(void)
{
    if (g_status_timer == NULL) return ESP_OK;
    return esp_timer_stop(g_status_timer);
}

void *ws_status_timer_handle_get(void)
{
    return g_status_timer;
}

/* Host-test reset: clears the module-static handle so the next
 * ws_status_timer_init() call re-creates the timer in a freshly
 * cleared mock slot table. Not for production use. */
void ws_status_timer_reset_handle_for_test(void)
{
    g_status_timer = NULL;
}
