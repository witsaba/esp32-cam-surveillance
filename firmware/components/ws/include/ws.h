/* ws.h — public API for the FW-13 WebSocket client component.
 *
 * Single entry point consumed by boot.c:153:
 *   esp_err_t ws_init(const config_t *cfg)
 * The boot orchestrator calls it as
 *   BOOT_CHECK_STEP(BOOT_STEP_WS_INIT, ws_init(cfg));
 *
 * The implementation (ws.c) registers the IP_EVENT_STA_GOT_IP
 * subscription + the WEBSOCKET_EVENT_CONNECTED/DISCONNECTED
 * handlers, then returns ESP_OK. The actual
 * `esp_websocket_client_start` happens in the IP-up handler
 * (lazy start per FW-13.1 + design #3756 §4) so DNS resolution
 * does not race STA association.
 *
 * Replaces the stub body at firmware/components/boot/
 * stub_inits.c:36-44. T-13-C is GREEN-only; the real impl
 * (esp_websocket_client_init + the start wiring) lands in
 * T-13-D..T-13-I.
 *
 * Host builds (`UNITY_HOST_BUILD` defined) include the mock link
 * headers BEFORE the IDF esp_websocket_client header. The mocks
 * swap every production call for the host-friendly equivalent
 * (mirrors the FW-08 wifi mock pattern).
 */
#pragma once

#include <stdint.h>

#include "config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-runtime metrics carrier struct. Populated by
 * `ws_runtime_metrics_collect()` (T-13-I GREEN). On host
 * the values come from the mocks; on device from the real IDF
 * APIs.
 *
 * Fields:
 *   uptime_us   — microseconds since boot (esp_timer_get_time()).
 *                 The status-frame builder converts to `uptime_s`.
 *   rssi_dbm    — station RSSI in dBm (esp_wifi_sta_get_rssi()).
 *                 Negative; -50 default on host per mock_esp_wifi.c.
 *   free_heap   — free heap bytes (esp_get_free_heap_size()).
 *                 ~200 KB default on host per mock_esp_system.c.
 *   fb_drops    — frame-buffer drops counter from the FW-11
 *                 capture task (capture_fb_drops_get()). Monotonic
 *                 once capture_task_start() runs.
 *   reconnects  — WS reconnect counter. FW-13 returns 0 (the
 *                 `ws_reconnects_get` stub` does not produce a
 *                 real counter yet). FW-14 owns the real impl
 *                 (per charter L1201 + design #3756 §1). */
typedef struct {
    int64_t  uptime_us;
    int32_t  rssi_dbm;
    uint32_t free_heap;
    uint32_t fb_drops;
    uint32_t reconnects;
} ws_runtime_metrics_t;

/* Initialize the WS client. Registers IP_EVENT_STA_GOT_IP
 * subscription + WEBSOCKET_EVENT_CONNECTED/DISCONNECTED handlers.
 * Does NOT call esp_websocket_client_start — the actual start
 * happens in the IP-up event handler (lazy start per FW-13.1 +
 * design #3756 §4).
 *
 * On host: returns ESP_OK after recording the config pointer to a
 * module-static (the production init sequence runs as the IDF
 * mocks resolve). The full production init sequence lands in
 * T-13-D GREEN (real impl).
 *
 * Returns:
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_ARG if cfg is NULL
 *   - ESP_ERR_* propagated from IDF init steps (esp_websocket
 *     _client_init, event handler register, esp_timer_create).
 *
 * On a forced-failure path via mock_init_returns_get(
 * BOOT_STEP_WS_INIT), returns the forced error (mirrors the
 * stub_inits.c behaviour; T-13-J replaces that stub). */
esp_err_t ws_init(const config_t *cfg);

#ifdef __cplusplus
}
#endif