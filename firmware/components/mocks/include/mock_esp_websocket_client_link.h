/* mock_esp_websocket_client_link.h — macro-redirect for the
 * esp_websocket_client API.
 *
 * Mirrors mock_esp_wifi_link.h: every `esp_websocket_*` call site is
 * replaced by the `mock_esp_websocket_client_*` target below when
 * `MOCK_WS_USE_REAL` is NOT defined. Production source (ws.c,
 * ws_event_handler.c, ws_status_timer.c) includes this BEFORE
 * `<esp_websocket_client.h>`.
 *
 * On host (UNITY_HOST_BUILD defined), the macros are active. On
 * device, the macros are inactive and the real `esp_websocket_*`
 * symbols from the IDF esp_websocket_client component are linked.
 *
 * Per #3751 API correction:
 *   - The function is `esp_websocket_register_events` (no `_client_`
 *     infix), 4-arg signature: `(handle, event_id, event_handler, arg)`.
 *   - The signature for `esp_websocket_client_close` in v1.8.0 is
 *     2-arg `(handle, timeout_ticks)` — NOT `(handle, code, reason, timeout)`.
 */
#pragma once

#include "mock_esp_websocket_client.h"

#ifndef MOCK_WS_USE_REAL

#define esp_websocket_client_init(cfg)                mock_esp_websocket_client_init(cfg)
#define esp_websocket_client_start(h)                 mock_esp_websocket_client_start(h)
#define esp_websocket_client_stop(h)                  mock_esp_websocket_client_stop(h)
#define esp_websocket_client_close(h, t)              mock_esp_websocket_client_close(h, t)
#define esp_websocket_client_send_text(h, d, l, t)    mock_esp_websocket_client_send_text(h, d, l, t)
#define esp_websocket_register_events(h, e, cb, arg)  mock_esp_websocket_register_events(h, e, cb, arg)

#endif  /* !MOCK_WS_USE_REAL */
