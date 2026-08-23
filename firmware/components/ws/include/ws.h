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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "esp_err.h"
#include "identity.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_websocket_client_link.h"
#else
#include "esp_websocket_client.h"
#endif

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

/* Production init body. Today (T-13-D GREEN) the symbol is
 * named ws_init_impl to avoid a duplicate-symbol linker error
 * against stub_inits.c::ws_init (the host runner compiles both).
 * stub_inits.c::ws_init dispatches here after honouring the
 * mock_init_returns_get short-circuit. T-13-J renames ws_init_impl
 * → ws_init and deletes the stub_inits.c body in one atomic
 * commit. */
esp_err_t ws_init_impl(const config_t *cfg);

/* Event handler seam (declared in ws_event_handler.c; here so
 * ws.c can install them idempotently). */
void ws_handle_set(esp_websocket_client_handle_t h);

/* Read-only accessor for the module-static handle. Tests use
 * this to drive the mock (esp_websocket_client_start fires
 * CONNECTED synchronously; the production on_ws_connected
 * emits the hello). On device the IP-up handler drives
 * _start, not tests. */
esp_websocket_client_handle_t ws_handle_get(void);

esp_err_t ws_event_handler_install(void);

/* ---------- FW-13.1 text-frame URI helpers ----------
 *
 * These live here so host tests can call them (the test surface
 * for FW-13.1 asserts on URI parsing + URI composition; the
 * runtime callers are ws.c::ws_init (compose) + the WS event
 * handlers (parse, future-proof)). */

/* Compose the WS URI from CONFIG_FIRMWARE_WS_URI_DEFAULT +
 * CONFIG_FIRMWARE_WS_PATH (both Kconfig-mirrored via -D cflags
 * on host). Writes at most `out_len` bytes to `out` (NUL-
 * terminated on success). The FW-13.4 URL-no-MAC guard
 * asserts no MAC substring appears in the composed URI.
 *
 * Returns:
 *   - ESP_OK on success (NUL-terminated in `out`).
 *   - ESP_ERR_INVALID_ARG if `out` is NULL or `out_len == 0`.
 *   - ESP_ERR_INVALID_SIZE if the composed URI would not fit
 *     in `out_len` bytes.
 *
 * Real impl lands in T-13-D GREEN. The T-13-C skeleton returns
 * ESP_OK + empty string. */
esp_err_t ws_url_build(char *out, size_t out_len);

/* Parse the URI's path component into `path_out` (NUL-
 * terminated on success). Accepts the `ws://host:port/path`
 * shape from IDF v5.5.3's esp_websocket_client_config_t.uri.
 *
 * Returns:
 *   - ESP_OK on success.
 *   - ESP_ERR_INVALID_ARG if `uri` or `path_out` is NULL.
 *   - ESP_ERR_INVALID_SIZE if `path_len` would not fit the
 *     parsed path.
 *
 * Real impl lands in T-13-D GREEN. The T-13-C skeleton returns
 * ESP_OK + empty string. */
esp_err_t ws_text_frame_parse_uri_path(const char *uri,
                                        char *path_out,
                                        size_t path_len);

/* ---------- FW-13.2 hello builder ---------- */

/* Build the hello frame JSON into `out`. Returns the number of
 * bytes written excluding the NUL terminator, or 0 if the buffer
 * was too small / identity is unparseable.
 *
 * Schema (per REQ-WS-002 + R-27):
 *   {"type":"hello","mac":"<12-hex>","name":"<nvs>",
 *    "description":"<nvs>","fw":"<version>",
 *    "caps":["jpeg","stream","identify"]}
 *
 * Real impl lands in T-13-E GREEN. The T-13-C skeleton returns 0
 * (no frame emitted). */
size_t ws_text_frame_build_hello(const device_identity_t *id,
                                  char *out, size_t out_len);

/* ---------- FW-13.6 status builder ---------- */

/* Build the status frame JSON into `out`. Returns the number of
 * bytes written excluding the NUL terminator, or 0 if the buffer
 * was too small.
 *
 * Schema (per REQ-WS-006 + R-27):
 *   {"type":"status","mac":"<12-hex>","name":"<nvs>",
 *    "uptime_s":<int>,"rssi_dbm":<int>,"free_heap":<int>,
 *    "fb_drops":<int>,"reconnects":<int>}
 *
 * Real impl lands in T-13-I GREEN. The T-13-C skeleton returns 0. */
size_t ws_text_frame_build_status(const ws_runtime_metrics_t *m,
                                   const device_identity_t *id,
                                   char *out, size_t out_len);

/* ---------- FW-13.5 status timer (T-13-H GREEN) ---------- */

/* Initialize the periodic 30 s status timer. Returns ESP_OK on
 * success. The timer is NOT started here — start happens on the
 * WEBSOCKET_EVENT_CONNECTED event handler. */
esp_err_t ws_status_timer_init(void);

/* Arm the periodic timer (start firing every
 * CONFIG_FIRMWARE_WS_STATUS_PERIOD_MS ms). */
esp_err_t ws_status_timer_start(void);

/* Disarm the periodic timer. */
esp_err_t ws_status_timer_stop(void);

/* Expose the timer handle so tests can advance it via
 * mock_esp_timer_advance_periodic(). Returns NULL before init. */
void *ws_status_timer_handle_get(void);

/* ---------- FW-13.6 runtime metrics (T-13-I GREEN) ---------- */

/* Populate `out` from the current runtime (timer + wifi rssi +
 * free heap + capture fb_drops + ws reconnects). */
void ws_runtime_metrics_collect(ws_runtime_metrics_t *out);

#ifdef __cplusplus
}
#endif