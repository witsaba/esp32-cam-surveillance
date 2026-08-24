/* ws_server.h — FW-16 device-as-server WebSocket endpoint.
 *
 * Registers the single-inbound-viewer streaming endpoint on the
 * EXISTING esp_http_server instance owned by the softap_sta
 * listener (no second listener, no outbound client):
 *
 *   GET <CONFIG_FIRMWARE_WS_PATH>  (.is_websocket = true)
 *
 * Lifecycle:
 *   ws_init() → ws_server_install()
 *     └─► subscribes IP_EVENT_STA_GOT_IP; on IP-up it reads the
 *       live STA httpd handle from the softap listener accessor
 *       (no softap→ws link edge — that would close the
 *       softap→ws→wifi→softap dependency cycle) and calls
 *       ws_server_register(hd) to attach the endpoint + capture
 *       the handle for async sends.
 *
 * Viewer-drop detection: the slot is validated via
 * httpd_ws_get_fd_info() on every probe, and any failed async
 * send frees it immediately — at 5 fps a vanished viewer is
 * re-attachable within one frame period, without server-wide
 * close-callback wiring.
 *
 * Single-viewer policy: exactly ONE WS session fd is tracked in
 * module state. A second handshake is answered with a short text
 * error frame and ESP_FAIL (socket closed by the server). The
 * server-wide max_open_sockets stays untouched — /whoami and
 * /snapshot keep working alongside a connected viewer.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef UNITY_HOST_BUILD
#include "mock_http_server_link.h"
#else
#include "esp_http_server.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Subscribe the IP-up attach hook (called once from ws_init).
 * Idempotent across duplicate GOT_IP events. */
esp_err_t ws_server_install(void);

/* Attach the /cams WebSocket endpoint onto `hd` (the live STA
 * httpd instance). Captures the handle for out-of-request async
 * sends (frames to the accepted viewer, rejection frames on a
 * second handshake). Returns the httpd_register_uri_handler
 * result. */
esp_err_t ws_server_register(httpd_handle_t hd);

/* True while a viewer session is active (fd captured at
 * handshake). */
bool ws_server_viewer_active(void);

/* Host-test reset: clears the captured handle + viewer fd and
 * reinstalls the disconnected sink. Not for production use. */
void ws_server_reset_for_test(void);

#ifdef __cplusplus
}
#endif
