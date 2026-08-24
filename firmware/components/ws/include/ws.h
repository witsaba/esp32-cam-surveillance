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
 * BOOT_STEP_WS_INIT), returns the forced error. The FW-03.2
 * bite-proof trips on this path. */
esp_err_t ws_init(const config_t *cfg);

/* Event handler seam (declared in ws_event_handler.c; here so
 * ws.c can install them idempotently). */
void ws_handle_set(esp_websocket_client_handle_t h);

/* Host-test reset — clears the idempotency flag so the next
 * ws_init → ws_event_handler_install call re-registers the
 * handlers. The mock's handler table is also reset on each
 * _init call; both resets are needed together to keep the
 * CONNECTED/DISCONNECTED subscriptions live across host test
 * cases. Not for production use. */
void ws_event_handler_reset_for_test(void);

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
 * for FW-13.1 asserts on URI parsing; the runtime caller was the
 * client-era lazy-start wiring). The ws_url_build() composer was
 * removed in FW-16 with the outbound-client lifecycle — server
 * mode registers a path endpoint, there is no outbound URL. */

/* Parse the URI's path component into `path_out` (NUL-
 * terminated on success). Accepts the `ws://host:port/path`
 * shape.
 *
 * Returns:
 *   - ESP_OK on success.
 *   - ESP_ERR_INVALID_ARG if `uri` or `path_out` is NULL.
 *   - ESP_ERR_INVALID_SIZE if `path_len` would not fit the
 *     parsed path. */
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

/* Host-test reset: clears the module-static status-timer handle
 * so the next ws_status_timer_init() call re-creates the timer
 * in a freshly cleared mock slot table. Not for production use. */
void ws_status_timer_reset_handle_for_test(void);

/* ---------- FW-13.6 runtime metrics (T-13-I GREEN) ---------- */

/* Populate `out` from the current runtime (timer + wifi rssi +
 * free heap + capture fb_drops + ws reconnects). */
void ws_runtime_metrics_collect(ws_runtime_metrics_t *out);

/* ---------- FW-16 server-mode sink seam ----------
 *
 * The device is a WebSocket SERVER (single inbound viewer on the
 * /cams endpoint). All frame emission — binary camera frames,
 * hello + status text — goes through this small function-pointer
 * table installed at viewer-accept time:
 *
 *   on-device  — ws_server.c binds hd+fd and sends via
 *                httpd_ws_send_frame_async()
 *   host tests — ws_sink_recorder (tests/host_include) records
 *                frames for byte-exact assertions
 *
 * With NO sink installed (boot default, and re-installed on
 * viewer close) the accessors report disconnected and sends fail
 * with ESP_ERR_INVALID_STATE — the stream task maps any failure
 * to the D4 drop-count path. */
typedef struct {
    esp_err_t (*send_bin)(const uint8_t *buf, size_t len);
    esp_err_t (*send_text)(const char *buf, size_t len);
    bool (*is_connected)(void);
} ws_sink_t;

/* Install `vt` as the active sink. NULL installs the built-in
 * disconnected stubs (also the boot default set by ws_init). */
void ws_sink_install(const ws_sink_t *vt);

/* True only while a viewer sink is installed and reports live. */
bool ws_sink_connected(void);

/* Push one complete binary message (camera frame). Returns
 * ESP_OK or the sink's error (never blocks). */
esp_err_t ws_sink_send_bin(const uint8_t *buf, size_t len);

/* Push one complete text message (hello / status JSON). */
esp_err_t ws_sink_send_text(const char *buf, size_t len);

#ifdef __cplusplus
}
#endif