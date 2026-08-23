/* mock_esp_websocket_client.h — host-side mock for the IDF
 * esp_websocket_client component (FW-13).
 *
 * Mirrors mock_esp_wifi.h: the production source (ws.c, ws_event_handler.c,
 * ws_status_timer.c) includes `mock_esp_websocket_client_link.h` which
 * `#define`s every `esp_websocket_*` symbol to the `mock_esp_websocket_client_*`
 * target below when `MOCK_WS_USE_REAL` is NOT defined.
 *
 * On host (UNITY_HOST_BUILD), the macros are active. On device, the
 * macros are inactive and the real `esp_websocket_*` symbols from the IDF
 * esp_websocket_client managed component are linked.
 *
 * The mock state machine:
 *   - `_init`: captures the config to `s_last_config`; allocates a
 *     sentinel non-NULL `s_handle`; honours the `s_inject_mac_into_url`
 *     Pass-11 gate (splices MAC hex into the URI path before capture).
 *   - `_start`: sets `s_started = s_connected = true`; synchronously
 *     invokes the registered CONNECTED handler (unless none was
 *     registered).
 *   - `_stop`: sets `s_stopped = true`, `s_connected = false`.
 *   - `_close`: sets `s_connected = false`; synchronously invokes the
 *     registered CLOSED handler.
 *   - `_send_text`: copies to `s_first_text_frame` if empty; appends
 *     to the ring buffer; increments `s_text_frame_count`; returns
 *     `len` on success.
 *   - `esp_websocket_register_events`: records the (event_id, cb, arg)
 *     tuple per subscription.
 *
 * API contract corrections from #3751:
 *   - The function is `esp_websocket_register_events` (4 args,
 *     single `event_id`), NOT `esp_websocket_client_register_events`.
 *   - The signature for `esp_websocket_client_close` in v1.8.0 is
 *     `(handle, timeout)` (2 args), not `(handle, code, reason, timeout)`.
 *     The mock matches the v1.8.0 IDF API exactly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Forward-declared types matching IDF's esp_websocket_client.h. On
 * host we don't pull in IDF; the mock only inspects a small subset
 * of `esp_websocket_client_config_t`. The handle is opaque. */
typedef struct mock_esp_websocket_client *esp_websocket_client_handle_t;

/* esp_websocket_transport_t (mirrors esp_websocket_client.h enum). */
typedef enum {
    WEBSOCKET_TRANSPORT_UNKNOWN = 0x0,
    WEBSOCKET_TRANSPORT_OVER_TCP,
    WEBSOCKET_TRANSPORT_OVER_SSL,
} esp_websocket_transport_t;

/* esp_websocket_event_id_t (mirrors esp_websocket_client.h enum).
 * The mock stores these by value; tests assert on exact values. */
typedef enum {
    WEBSOCKET_EVENT_ANY        = -1,
    WEBSOCKET_EVENT_ERROR      = 0,
    WEBSOCKET_EVENT_CONNECTED  = 2,
    WEBSOCKET_EVENT_DISCONNECTED,
    WEBSOCKET_EVENT_DATA,
    WEBSOCKET_EVENT_CLOSED,
} esp_websocket_event_id_t;

/* esp_event_handler_t (mirrors esp_event.h — already declared in
 * mock_esp_event.h but we forward-declare locally to avoid forcing
 * the ws component to include mock_esp_event_link.h).
 *
 * The production source binds its handlers via
 * `esp_websocket_register_events(handle, event_id, cb, arg)` where
 * `cb` has the IDF `esp_event_handler_t` shape:
 *     void cb(void *arg, esp_event_base_t base, int32_t id, void *data);
 */
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *handler_arg,
                                     esp_event_base_t base,
                                     int32_t event_id,
                                     void *event_data);

/* esp_websocket_client_config_t — minimal stub for host. The mock
 * captures (uri, transport, disable_auto_reconnect, buffer_size,
 * ping_interval_sec, pingpong_timeout_sec, network_timeout_ms,
 * task_stack). Other fields are opaque placeholders. */
typedef struct {
    const char *uri;
    const char *host;
    int         port;
    const char *path;
    bool        disable_auto_reconnect;
    int         buffer_size;
    size_t      ping_interval_sec;
    int         pingpong_timeout_sec;
    int         network_timeout_ms;
    int         task_stack;
    esp_websocket_transport_t transport;
    const char *headers;
    const char *subprotocol;
    const char *user_agent;
    /* Opaque padding — the mock never reads past `transport`. */
    int         _placeholder[16];
} esp_websocket_client_config_t;

/* Text frame buffer capacity. Sized to comfortably hold a single
 * hello frame (~256 bytes) + a single status frame (~384 bytes). */
#define MOCK_WS_TEXT_FRAME_CAP 1024

/* Text frame ring buffer capacity. Holds the most recent N frames
 * for `mock_esp_websocket_client_get_text_frame_at`. */
#define MOCK_WS_TEXT_FRAME_RING_CAP 16

/* ---------- primable state (test entries) ---------- */

/* Pass-11 gate — when true, `_init` splices the MAC hex substring
 * (provided via `mock_esp_websocket_client_set_mac_for_inject`)
 * into the `_init`-captured URI path. Mirrors the
 * `WS_TEST_STUB_INJECT_MAC_INTO_URL=1` build flag. */
void mock_esp_websocket_client_set_inject_mac_into_url(bool inject);
bool mock_esp_websocket_client_get_inject_mac_into_url(void);

/* MAC bytes used for the inject-MAC splice (default 00:00:00:00:00:00). */
void mock_esp_websocket_client_set_mac_for_inject(const uint8_t mac[6]);

/* Primed text frame data to copy into `s_first_text_frame` on the
 * first `_send_text` (default empty — captures whatever production
 * sent). Used by tests to seed the buffer BEFORE the production
 * call so the assertion path is deterministic. */
void mock_esp_websocket_client_set_first_text_frame(const char *data, size_t len);

/* Reset all module-static state between tests. */
void mock_esp_websocket_client_reset_for_test(void);

/* ---------- inspection (test entries) ---------- */

/* Pointer to the most recent `_init` URI captured (NULL if never called). */
const char *mock_esp_websocket_client_get_last_uri(void);

/* Pointer to the full last config struct (NULL if never called). */
const esp_websocket_client_config_t *mock_esp_websocket_client_get_last_config(void);

/* The transport of the most recent `_init`. */
esp_websocket_transport_t mock_esp_websocket_client_get_transport(void);

/* The disable_auto_reconnect flag of the most recent `_init`. */
bool mock_esp_websocket_client_get_disable_auto_reconnect(void);

/* Copy the first text frame captured by `_send_text` into `out`.
 * Returns the number of bytes copied (excluding NUL). 0 if no
 * frame was sent yet. `out_len` must be >= 1 to receive the NUL. */
esp_err_t mock_esp_websocket_client_get_first_text_frame(char *out, size_t out_len);

/* Number of `_send_text` calls observed since last reset. */
size_t mock_esp_websocket_client_get_text_frame_count(void);

/* Copy the text frame at `idx` (0-based) into `out`. Returns the
 * number of bytes copied (excluding NUL). Returns 0 if `idx` is
 * out of range or no frames have been sent. */
esp_err_t mock_esp_websocket_client_get_text_frame_at(size_t idx, char *out, size_t out_len);

/* Start/stop state. */
bool mock_esp_websocket_client_get_started(void);
bool mock_esp_websocket_client_get_stopped(void);
bool mock_esp_websocket_client_get_connected(void);

/* Call counters. */
size_t mock_esp_websocket_client_init_call_count(void);
size_t mock_esp_websocket_client_start_call_count(void);
size_t mock_esp_websocket_client_stop_call_count(void);
size_t mock_esp_websocket_client_close_call_count(void);
size_t mock_esp_websocket_client_send_text_call_count(void);
size_t mock_esp_websocket_client_register_events_call_count(void);

/* ---------- test entry: fire registered event handler ---------- */

/* Synchronously invoke the handler registered via
 * `esp_websocket_register_events` for `event_id`. If no handler was
 * registered for that event, returns ESP_ERR_NOT_FOUND. Mirrors
 * mock_esp_event_fire_handler and mock_esp_timer_fire_callback. */
esp_err_t mock_esp_websocket_client_fire_event(esp_websocket_event_id_t event_id);

/* Fire DISCONNECTED specifically — convenience for FW-13.5 S2. */
void mock_esp_websocket_client_fire_disconnected(void);

/* ---------- mock targets (link-header redirects) ---------- */

esp_websocket_client_handle_t mock_esp_websocket_client_init(
    const esp_websocket_client_config_t *config);

esp_err_t mock_esp_websocket_client_start(esp_websocket_client_handle_t client);

esp_err_t mock_esp_websocket_client_stop(esp_websocket_client_handle_t client);

esp_err_t mock_esp_websocket_client_close(esp_websocket_client_handle_t client,
                                            int timeout_ticks);

int mock_esp_websocket_client_send_text(esp_websocket_client_handle_t client,
                                         const char *data, int len,
                                         int timeout_ticks);

esp_err_t mock_esp_websocket_register_events(
    esp_websocket_client_handle_t client,
    esp_websocket_event_id_t event,
    esp_event_handler_t event_handler,
    void *event_handler_arg);
