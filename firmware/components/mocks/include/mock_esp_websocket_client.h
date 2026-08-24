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
 *
 * FW-14 additions (auto-reconnect backoff):
 *   - `esp_websocket_client_set_reconnect_timeout` redirect captures
 *     the requested delay; tests read it via
 *     `mock_esp_websocket_client_get_last_reconnect_timeout_ms()`
 *     (+ call count).
 *   - `mock_esp_websocket_client_fire_closed(code)` fires the
 *     CLOSED handler with a populated `close_status_code`. The
 *     pinned v1.8.0 component has NO
 *     `esp_websocket_client_get_close_code()` accessor — the close
 *     status code arrives on the event payload
 *     (`esp_websocket_event_data_t.close_status_code`, populated by
 *     the dispatcher for every event), so production derives
 *     clean-CLOSE from the payload, and this mock carries it there.
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
    /* FW-14 — reconnect-policy config fields (mirrors v1.8.0). */
    bool        enable_close_reconnect;
    int         reconnect_timeout_ms;
    /* Opaque padding — the mock never reads past `transport`. */
    int         _placeholder[16];
} esp_websocket_client_config_t;

/* esp_websocket_event_data_t — minimal stub mirroring the pinned
 * v1.8.0 layout for the fields production code reads on host.
 * The real dispatcher populates `close_status_code` for EVERY
 * event from client state; tests prime it via
 * `mock_esp_websocket_client_fire_closed(code)`. */
typedef struct {
    const char *data_ptr;
    int         data_len;
    int         close_status_code;  /*!< RFC 6455 close status code (0 if none / client-initiated) */
} esp_websocket_event_data_t;

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

/* Fire CLOSED with the given RFC 6455 close status code. The
 * handler receives a stack `esp_websocket_event_data_t` whose
 * `close_status_code` is `code` (mirrors the v1.8.0 dispatcher,
 * which populates the payload for every event). */
void mock_esp_websocket_client_fire_closed(int close_status_code);

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

/* FW-14 — reconnect-delay setter redirect (v1.8.0 signature).
 * Captures `reconnect_timeout_ms` for test inspection. Note: the
 * real v1.8.0 client REJECTS this call with ESP_ERR_INVALID_STATE
 * when auto_reconnect is disabled; the mock accepts it so tests
 * can assert on the requested schedule (the ws_backoff module's
 * own state is authoritative at runtime). */
esp_err_t mock_esp_websocket_client_set_reconnect_timeout(
    esp_websocket_client_handle_t client,
    int reconnect_timeout_ms);

/* ---------- FW-14 inspection (test entries) ---------- */

/* Last value passed to set_reconnect_timeout (-1 if never called). */
int mock_esp_websocket_client_get_last_reconnect_timeout_ms(void);

/* Number of set_reconnect_timeout calls observed since last reset. */
size_t mock_esp_websocket_client_set_reconnect_timeout_call_count(void);

/* enable_close_reconnect flag of the most recent `_init`. */
bool mock_esp_websocket_client_get_enable_close_reconnect(void);

/* reconnect_timeout_ms config field of the most recent `_init`. */
int mock_esp_websocket_client_get_config_reconnect_timeout_ms(void);

/* ---------- FW-15 binary-send surface ---------- */

/* Binary frame ring capacity (frames, newest at head) and the
 * per-slot payload cap. Sized so a full fragmented 48 KB frame
 * (3 × 16 KB parts) plus slack reassembles byte-exactly in
 * tests. Host-only memory; device never links this file. */
#define MOCK_WS_BIN_RING_CAP 8
#define MOCK_WS_BIN_PART_CAP 65536

/* Primable failure injection: when set to N >= 0, the Nth
 * binary-send operation (counting send_bin / send_bin_partial /
 * send_cont_msg / send_fin together since reset) returns -1 and
 * records NOTHING. -1 disables. Used by REQ-ST-007 drain-drop
 * -count tests to simulate a dead socket mid-frame.
 * fail_all_set(true) makes EVERY binary-send op fail regardless
 * of index (persistent dead socket). */
void mock_esp_websocket_client_fail_at_index_set(int idx);
void mock_esp_websocket_client_fail_all_set(bool fail_all);

/* Total binary-send operations observed (all four verbs), and
 * per-verb counters. Reset by reset_for_test(). */
size_t mock_esp_websocket_client_bin_op_call_count(void);
size_t mock_esp_websocket_client_send_bin_call_count(void);
size_t mock_esp_websocket_client_send_bin_partial_call_count(void);
size_t mock_esp_websocket_client_send_cont_msg_call_count(void);
size_t mock_esp_websocket_client_send_fin_call_count(void);

/* Copy binary frame `idx` (0 = OLDEST recorded) into `out`.
 * Writes the wire opcode (0x2 binary / 0x0 continuation) and
 * FIN flag, the payload length, and up to out_cap payload
 * bytes. Returns ESP_OK; ESP_ERR_NOT_FOUND when idx is out of
 * range; ESP_ERR_INVALID_SIZE when out_cap < len (bytes are
 * still copied truncated — assert len first for byte-exact
 * reassembly). */
esp_err_t mock_esp_websocket_client_get_bin_frame_at(
    size_t idx, uint8_t *opcode, bool *fin,
    uint8_t *out, size_t out_cap, size_t *out_len);

/* ---------- FW-15 mock targets (link-header redirects) ----------
 * Mirrors the v1.8.0 IDF signatures: int return (payload length,
 * or -1 on failure); send_fin returns 0/-1. Each call appends one
 * {opcode, fin, data copy} slot to the binary ring:
 *   send_bin         → opcode 0x2, fin=1
 *   send_bin_partial → opcode 0x2, fin=0   (first fragment)
 *   send_cont_msg    → opcode 0x0, fin=0   (middle fragments)
 *   send_fin         → opcode 0x0, fin=1, len=0 (terminator)
 */
int mock_esp_websocket_client_send_bin(
    esp_websocket_client_handle_t client,
    const char *data, int len, int timeout_ticks);
int mock_esp_websocket_client_send_bin_partial(
    esp_websocket_client_handle_t client,
    const char *data, int len, int timeout_ticks);
int mock_esp_websocket_client_send_cont_msg(
    esp_websocket_client_handle_t client,
    const char *data, int len, int timeout_ticks);
int mock_esp_websocket_client_send_fin(
    esp_websocket_client_handle_t client, int timeout_ticks);
