/* mock_esp_websocket_client.c — implementation of the
 * esp_websocket_client mock (FW-13).
 *
 * Mirrors mock_esp_wifi.c pattern: module-static state + per-IDF-symbol
 * mock functions + test-entry getters/setters. The mock state machine
 * is described in `mock_esp_websocket_client.h`.
 *
 * Important: the link-header redirect (mock_esp_websocket_client_link.h)
 * `#define`s every `esp_websocket_*` symbol to its mock target here.
 * Production source (ws.c, ws_event_handler.c, ws_status_timer.c) only
 * ever sees the mock symbols at link time.
 */
#include "mock_esp_websocket_client.h"

#include <string.h>

/* Define the opaque struct so the mock can hold a sentinel handle
 * in static storage. Production source only ever sees the typedef
 * from mock_esp_websocket_client.h and never touches the fields. */
struct mock_esp_websocket_client {
    int _placeholder;
};

/* ---------- module-static state ---------- */

static esp_websocket_client_config_t s_last_config;
static bool                          s_last_config_valid = false;

static struct mock_esp_websocket_client s_handle_storage;
static esp_websocket_client_handle_t    s_handle = NULL;

static bool s_started    = false;
static bool s_stopped    = false;
static bool s_connected  = false;

static char  s_first_text_frame[MOCK_WS_TEXT_FRAME_CAP];
static size_t s_first_text_frame_len = 0;

/* Ring buffer of the most recent N text frames (newest at head). */
typedef struct {
    char  data[MOCK_WS_TEXT_FRAME_CAP];
    size_t len;
} text_frame_slot_t;

static text_frame_slot_t s_text_ring[MOCK_WS_TEXT_FRAME_RING_CAP];
static size_t            s_text_ring_head = 0;  /* next write slot */
static size_t            s_text_frame_count = 0;

/* Per-event-id handler registrations. WEBSOCKET_EVENT_ANY (-1)
 * is the special wildcard slot (used by some callers); named
 * events get their own slot. */
static esp_event_handler_t s_handlers[WEBSOCKET_EVENT_CLOSED + 1];
static void              *s_handler_args[WEBSOCKET_EVENT_CLOSED + 1];
static bool               s_handler_registered[WEBSOCKET_EVENT_CLOSED + 1];

/* Pass-11 gate state. */
static bool    s_inject_mac_into_url = false;
static uint8_t s_mac_for_inject[6]   = {0,0,0,0,0,0};
static char    s_injected_uri[MOCK_WS_TEXT_FRAME_CAP];

/* Primed text frame (test entry). */
static char  s_primed_frame[MOCK_WS_TEXT_FRAME_CAP];
static size_t s_primed_frame_len = 0;
static bool   s_has_primed_frame = false;

/* Call counters. */
static size_t g_init_count           = 0;
static size_t g_start_count          = 0;
static size_t g_stop_count           = 0;
static size_t g_close_count          = 0;
static size_t g_send_text_count      = 0;
static size_t g_register_events_count = 0;

/* FW-14 — set_reconnect_timeout capture. */
static int   g_last_reconnect_timeout_ms = -1;
static bool  g_reconnect_timeout_valid   = false;
static size_t g_set_reconnect_timeout_count = 0;

/* FW-15 — binary-send surface: ring {opcode, fin, data copy},
 * per-verb counters, and a primable fail-at-index injection. */
typedef struct {
    uint8_t opcode;
    bool    fin;
    size_t  len;
    uint8_t data[MOCK_WS_BIN_PART_CAP];
} bin_frame_slot_t;

static bin_frame_slot_t s_bin_ring[MOCK_WS_BIN_RING_CAP];
static size_t           s_bin_ring_count = 0;   /* frames recorded (saturates at ring cap) */
static size_t           s_bin_ring_total = 0;   /* total recorded ever (for idx math) */

static size_t g_send_bin_count          = 0;
static size_t g_send_bin_partial_count  = 0;
static size_t g_send_cont_msg_count     = 0;
static size_t g_send_fin_count          = 0;
static int    g_bin_fail_at_index       = -1;    /* -1 disabled */
static size_t g_bin_op_count            = 0;     /* all four verbs since reset */

/* ---------- primable state (test entries) ---------- */

void mock_esp_websocket_client_set_inject_mac_into_url(bool inject)
{
    s_inject_mac_into_url = inject;
}

bool mock_esp_websocket_client_get_inject_mac_into_url(void)
{
    return s_inject_mac_into_url;
}

void mock_esp_websocket_client_set_mac_for_inject(const uint8_t mac[6])
{
    if (mac) memcpy(s_mac_for_inject, mac, sizeof(s_mac_for_inject));
}

void mock_esp_websocket_client_set_first_text_frame(const char *data, size_t len)
{
    if (data && len > 0) {
        size_t n = len < sizeof(s_primed_frame) - 1 ? len : sizeof(s_primed_frame) - 1;
        memcpy(s_primed_frame, data, n);
        s_primed_frame[n] = '\0';
        s_primed_frame_len = n;
        s_has_primed_frame = true;
    } else {
        s_primed_frame[0] = '\0';
        s_primed_frame_len = 0;
        s_has_primed_frame = false;
    }
}

void mock_esp_websocket_client_reset_for_test(void)
{
    memset(&s_last_config, 0, sizeof(s_last_config));
    s_last_config_valid      = false;
    s_handle                 = NULL;
    s_started                = false;
    s_stopped                = false;
    s_connected              = false;
    s_first_text_frame[0]    = '\0';
    s_first_text_frame_len   = 0;
    memset(s_text_ring, 0, sizeof(s_text_ring));
    s_text_ring_head         = 0;
    s_text_frame_count       = 0;
    memset(s_handlers, 0, sizeof(s_handlers));
    memset(s_handler_args, 0, sizeof(s_handler_args));
    memset(s_handler_registered, 0, sizeof(s_handler_registered));
    s_inject_mac_into_url    = false;
    memset(s_mac_for_inject, 0, sizeof(s_mac_for_inject));
    s_injected_uri[0]        = '\0';
    s_primed_frame[0]        = '\0';
    s_primed_frame_len       = 0;
    s_has_primed_frame       = false;
    g_init_count             = 0;
    g_start_count            = 0;
    g_stop_count             = 0;
    g_close_count            = 0;
    g_send_text_count        = 0;
    g_register_events_count  = 0;
    g_last_reconnect_timeout_ms = -1;
    g_reconnect_timeout_valid   = false;
    g_set_reconnect_timeout_count = 0;

    /* FW-15 — binary surface. */
    memset(s_bin_ring, 0, sizeof(s_bin_ring));
    s_bin_ring_count     = 0;
    s_bin_ring_total     = 0;
    g_send_bin_count     = 0;
    g_send_bin_partial_count = 0;
    g_send_cont_msg_count    = 0;
    g_send_fin_count         = 0;
    g_bin_fail_at_index      = -1;
    g_bin_op_count           = 0;
}

/* ---------- inspection (test entries) ---------- */

const char *mock_esp_websocket_client_get_last_uri(void)
{
    return s_last_config_valid ? s_last_config.uri : NULL;
}

const esp_websocket_client_config_t *mock_esp_websocket_client_get_last_config(void)
{
    return s_last_config_valid ? &s_last_config : NULL;
}

esp_websocket_transport_t mock_esp_websocket_client_get_transport(void)
{
    return s_last_config_valid ? s_last_config.transport : WEBSOCKET_TRANSPORT_UNKNOWN;
}

bool mock_esp_websocket_client_get_disable_auto_reconnect(void)
{
    return s_last_config_valid ? s_last_config.disable_auto_reconnect : false;
}

esp_err_t mock_esp_websocket_client_get_first_text_frame(char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    if (s_first_text_frame_len == 0) {
        out[0] = '\0';
        return ESP_OK;
    }
    size_t n = s_first_text_frame_len < out_len - 1
                 ? s_first_text_frame_len
                 : out_len - 1;
    memcpy(out, s_first_text_frame, n);
    out[n] = '\0';
    return ESP_OK;
}

size_t mock_esp_websocket_client_get_text_frame_count(void)
{
    return s_text_frame_count;
}

esp_err_t mock_esp_websocket_client_get_text_frame_at(size_t idx, char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    if (idx >= s_text_ring_head) {
        out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    /* Ring buffer: newest is at head-1, oldest is at 0. */
    size_t slot = s_text_ring_head - 1 - idx;
    const text_frame_slot_t *s = &s_text_ring[slot];
    size_t n = s->len < out_len - 1 ? s->len : out_len - 1;
    memcpy(out, s->data, n);
    out[n] = '\0';
    return ESP_OK;
}

bool mock_esp_websocket_client_get_started(void)   { return s_started;   }
bool mock_esp_websocket_client_get_stopped(void)   { return s_stopped;   }
bool mock_esp_websocket_client_get_connected(void) { return s_connected; }

size_t mock_esp_websocket_client_init_call_count(void)            { return g_init_count; }
size_t mock_esp_websocket_client_start_call_count(void)           { return g_start_count; }
size_t mock_esp_websocket_client_stop_call_count(void)            { return g_stop_count; }
size_t mock_esp_websocket_client_close_call_count(void)           { return g_close_count; }
size_t mock_esp_websocket_client_send_text_call_count(void)       { return g_send_text_count; }
size_t mock_esp_websocket_client_register_events_call_count(void) { return g_register_events_count; }

/* ---------- test entry: fire registered event handler ---------- */

/* Helper — invokes a registered handler if present. Looks up the
 * specific event_id first; falls back to the wildcard slot. Returns
 * ESP_OK if a handler was found and invoked. `event_data` is passed
 * through verbatim (NULL for most events; CLOSED carries a stack
 * esp_websocket_event_data_t via mock_..._fire_closed). */
static esp_err_t fire_handler_for_data(esp_websocket_event_id_t event_id,
                                        void *event_data)
{
    esp_event_handler_t cb = NULL;
    void *arg = NULL;
    if ((int)event_id >= 0 && (int)event_id <= WEBSOCKET_EVENT_CLOSED) {
        if (s_handler_registered[event_id]) {
            cb = s_handlers[event_id];
            arg = s_handler_args[event_id];
        }
    }
    if (!cb && s_handler_registered[0]) {  /* WEBSOCKET_EVENT_ANY stored at index 0 */
        cb = s_handlers[0];
        arg = s_handler_args[0];
    }
    if (!cb) return ESP_ERR_NOT_FOUND;
    /* The IDF passes the WS event base as the second arg, and the
     * event_id as the third. */
    cb(arg, NULL, (int32_t)event_id, event_data);
    return ESP_OK;
}

/* NULL-event_data wrapper kept for the existing call sites. */
static esp_err_t fire_handler_for(esp_websocket_event_id_t event_id)
{
    return fire_handler_for_data(event_id, NULL);
}

esp_err_t mock_esp_websocket_client_fire_event(esp_websocket_event_id_t event_id)
{
    return fire_handler_for(event_id);
}

void mock_esp_websocket_client_fire_disconnected(void)
{
    (void)fire_handler_for(WEBSOCKET_EVENT_DISCONNECTED);
}

void mock_esp_websocket_client_fire_closed(int close_status_code)
{
    esp_websocket_event_data_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.close_status_code = close_status_code;
    (void)fire_handler_for_data(WEBSOCKET_EVENT_CLOSED, &ev);
}

/* ---------- mock targets (link-header redirects) ---------- */

/* Helper — formats the MAC as 12-char lowercase hex. */
static void mac_to_hex12(const uint8_t mac[6], char out[13])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        out[i * 2]     = hex[(mac[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[mac[i] & 0x0F];
    }
    out[12] = '\0';
}

esp_websocket_client_handle_t mock_esp_websocket_client_init(
    const esp_websocket_client_config_t *config)
{
    g_init_count++;
    if (!config) return NULL;

    /* Pass-11 tripwire: when the test injects MAC into the URL,
     * splice the MAC hex substring into the URI path before
     * capture. This makes the ws_url_build guard trip during
     * FW-13.4 Pass-11. */
    if (s_inject_mac_into_url && config->uri) {
        char mac_hex[13];
        mac_to_hex12(s_mac_for_inject, mac_hex);

        /* Find the path component (after the third '/'). */
        const char *p = config->uri;
        int slash_count = 0;
        const char *path_start = NULL;
        while (*p) {
            if (*p == '/') {
                slash_count++;
                if (slash_count == 3) {
                    path_start = p;
                    break;
                }
            }
            p++;
        }

        if (path_start) {
            /* Splice MAC into the path. */
            int prefix_len = (int)(path_start - config->uri);
            int suffix_len = (int)strlen(path_start);
            int total = prefix_len + 12 + suffix_len;
            if ((size_t)total < sizeof(s_injected_uri)) {
                memcpy(s_injected_uri, config->uri, prefix_len);
                memcpy(s_injected_uri + prefix_len, mac_hex, 12);
                memcpy(s_injected_uri + prefix_len + 12,
                       path_start, suffix_len + 1);  /* include NUL */
                /* Capture the modified URI. */
                s_last_config = *config;
                s_last_config.uri = s_injected_uri;
                s_last_config_valid = true;
            } else {
                /* Path too long — capture unmodified. */
                s_last_config = *config;
                s_last_config_valid = true;
            }
        } else {
            /* No path component — append MAC. */
            size_t base_len = strlen(config->uri);
            if (base_len + 13 < sizeof(s_injected_uri)) {
                memcpy(s_injected_uri, config->uri, base_len);
                memcpy(s_injected_uri + base_len, mac_hex, 13);  /* +NUL */
                s_last_config = *config;
                s_last_config.uri = s_injected_uri;
                s_last_config_valid = true;
            } else {
                s_last_config = *config;
                s_last_config_valid = true;
            }
        }
    } else {
        s_last_config = *config;
        s_last_config_valid = true;
    }

    /* Reset state-machine flags; the previous run's started/stopped
     * state must not bleed into the new session. */
    s_started   = false;
    s_stopped   = false;
    s_connected = false;
    s_first_text_frame[0] = '\0';
    s_first_text_frame_len = 0;
    memset(s_text_ring, 0, sizeof(s_text_ring));
    s_text_ring_head   = 0;
    s_text_frame_count = 0;

    /* Clear any prior handler registrations. */
    memset(s_handlers, 0, sizeof(s_handlers));
    memset(s_handler_args, 0, sizeof(s_handler_args));
    memset(s_handler_registered, 0, sizeof(s_handler_registered));

    /* Allocate the sentinel handle (always non-NULL on success). */
    s_handle = &s_handle_storage;
    return s_handle;
}

esp_err_t mock_esp_websocket_client_start(esp_websocket_client_handle_t client)
{
    g_start_count++;
    if (!client || client != s_handle) return ESP_ERR_INVALID_ARG;
    s_started   = true;
    s_connected = true;
    /* Synchronously fire CONNECTED — production ws.c uses the same
     * synchronous pattern via the IDF event loop. */
    (void)fire_handler_for(WEBSOCKET_EVENT_CONNECTED);
    return ESP_OK;
}

esp_err_t mock_esp_websocket_client_stop(esp_websocket_client_handle_t client)
{
    g_stop_count++;
    if (!client || client != s_handle) return ESP_ERR_INVALID_ARG;
    s_stopped   = true;
    s_connected = false;
    return ESP_OK;
}

esp_err_t mock_esp_websocket_client_close(esp_websocket_client_handle_t client,
                                            int timeout_ticks)
{
    g_close_count++;
    if (!client || client != s_handle) return ESP_ERR_INVALID_ARG;
    (void)timeout_ticks;  /* mock fires synchronously */
    s_connected = false;
    (void)fire_handler_for(WEBSOCKET_EVENT_CLOSED);
    return ESP_OK;
}

int mock_esp_websocket_client_send_text(esp_websocket_client_handle_t client,
                                         const char *data, int len,
                                         int timeout_ticks)
{
    g_send_text_count++;
    if (!client || client != s_handle || !data || len < 0) return -1;
    (void)timeout_ticks;

    size_t ulen = (size_t)len;

    /* If a primed frame was set AND we haven't captured anything yet,
     * use it (tests use this to seed deterministic content). */
    if (s_first_text_frame_len == 0 && s_has_primed_frame) {
        size_t n = s_primed_frame_len < sizeof(s_first_text_frame) - 1
                     ? s_primed_frame_len
                     : sizeof(s_first_text_frame) - 1;
        memcpy(s_first_text_frame, s_primed_frame, n);
        s_first_text_frame[n] = '\0';
        s_first_text_frame_len = n;
        s_has_primed_frame = false;  /* consume once */
        /* Still append the actual call's data to the ring buffer so
         * count/ring tests see the production call. */
    }

    if (s_first_text_frame_len == 0 && ulen > 0) {
        size_t n = ulen < sizeof(s_first_text_frame) - 1
                     ? ulen
                     : sizeof(s_first_text_frame) - 1;
        memcpy(s_first_text_frame, data, n);
        s_first_text_frame[n] = '\0';
        s_first_text_frame_len = n;
    }

    /* Append to ring buffer (newest at head). */
    if (s_text_ring_head < MOCK_WS_TEXT_FRAME_RING_CAP) {
        text_frame_slot_t *slot = &s_text_ring[s_text_ring_head++];
        size_t n = ulen < sizeof(slot->data) - 1
                     ? ulen
                     : sizeof(slot->data) - 1;
        if (n > 0) memcpy(slot->data, data, n);
        slot->data[n] = '\0';
        slot->len = n;
    } else {
        /* Shift left to make room, dropping the oldest. */
        memmove(s_text_ring, s_text_ring + 1,
                (MOCK_WS_TEXT_FRAME_RING_CAP - 1) * sizeof(text_frame_slot_t));
        text_frame_slot_t *slot = &s_text_ring[MOCK_WS_TEXT_FRAME_RING_CAP - 1];
        size_t n = ulen < sizeof(slot->data) - 1
                     ? ulen
                     : sizeof(slot->data) - 1;
        if (n > 0) memcpy(slot->data, data, n);
        slot->data[n] = '\0';
        slot->len = n;
    }

    s_text_frame_count++;
    return (int)ulen;
}

esp_err_t mock_esp_websocket_register_events(
    esp_websocket_client_handle_t client,
    esp_websocket_event_id_t event,
    esp_event_handler_t event_handler,
    void *event_handler_arg)
{
    g_register_events_count++;
    if (!client || client != s_handle || !event_handler) return ESP_ERR_INVALID_ARG;

    /* WEBSOCKET_EVENT_ANY (-1) → wildcard slot at index 0. */
    if (event == WEBSOCKET_EVENT_ANY) {
        s_handlers[0] = event_handler;
        s_handler_args[0] = event_handler_arg;
        s_handler_registered[0] = true;
        return ESP_OK;
    }

    if ((int)event < 0 || (int)event > WEBSOCKET_EVENT_CLOSED) {
        return ESP_ERR_INVALID_ARG;
    }
    s_handlers[event] = event_handler;
    s_handler_args[event] = event_handler_arg;
    s_handler_registered[event] = true;
    return ESP_OK;
}

/* ---------- FW-14 mock targets + inspection ---------- */

esp_err_t mock_esp_websocket_client_set_reconnect_timeout(
    esp_websocket_client_handle_t client,
    int reconnect_timeout_ms)
{
    (void)client;
    g_set_reconnect_timeout_count++;
    g_last_reconnect_timeout_ms = reconnect_timeout_ms;
    g_reconnect_timeout_valid   = true;
    /* The real v1.8.0 client returns ESP_ERR_INVALID_STATE when
     * auto_reconnect is disabled; the mock accepts so tests can
     * assert the requested schedule. */
    return ESP_OK;
}

int mock_esp_websocket_client_get_last_reconnect_timeout_ms(void)
{
    return g_reconnect_timeout_valid ? g_last_reconnect_timeout_ms : -1;
}

size_t mock_esp_websocket_client_set_reconnect_timeout_call_count(void)
{
    return g_set_reconnect_timeout_count;
}

bool mock_esp_websocket_client_get_enable_close_reconnect(void)
{
    return s_last_config_valid ? s_last_config.enable_close_reconnect : false;
}

int mock_esp_websocket_client_get_config_reconnect_timeout_ms(void)
{
    return s_last_config_valid ? s_last_config.reconnect_timeout_ms : -1;
}

/* ---------- FW-15 binary-send surface ---------- */

/* Shared recorder. Returns -1 when the fail-at-index injection
 * fires (nothing recorded); otherwise appends the frame to the
 * ring (oldest-first, bounded) and returns `rc`. */
static int bin_record(uint8_t opcode, bool fin,
                      const char *data, int len, int rc)
{
    if (!data || len < 0) return -1;
    if (g_bin_fail_at_index >= 0 &&
        (size_t)g_bin_fail_at_index == g_bin_op_count) {
        g_bin_op_count++;
        return -1;
    }
    g_bin_op_count++;

    bin_frame_slot_t *slot = NULL;
    if (s_bin_ring_count < MOCK_WS_BIN_RING_CAP) {
        slot = &s_bin_ring[s_bin_ring_count++];
    } else {
        /* Ring full: drop the oldest by shifting left. */
        memmove(s_bin_ring, s_bin_ring + 1,
                (MOCK_WS_BIN_RING_CAP - 1) * sizeof(bin_frame_slot_t));
        slot = &s_bin_ring[MOCK_WS_BIN_RING_CAP - 1];
    }
    slot->opcode = opcode;
    slot->fin    = fin;
    size_t n = (len >= 0 && (size_t)len <= MOCK_WS_BIN_PART_CAP)
                 ? (size_t)len : MOCK_WS_BIN_PART_CAP;
    if (n > 0) memcpy(slot->data, data, n);
    slot->len = n;
    s_bin_ring_total++;
    return rc;
}

int mock_esp_websocket_client_send_bin(
    esp_websocket_client_handle_t client,
    const char *data, int len, int timeout_ticks)
{
    (void)client; (void)timeout_ticks;
    g_send_bin_count++;
    return bin_record(0x2, true, data, len, len);
}

int mock_esp_websocket_client_send_bin_partial(
    esp_websocket_client_handle_t client,
    const char *data, int len, int timeout_ticks)
{
    (void)client; (void)timeout_ticks;
    g_send_bin_partial_count++;
    return bin_record(0x2, false, data, len, len);
}

int mock_esp_websocket_client_send_cont_msg(
    esp_websocket_client_handle_t client,
    const char *data, int len, int timeout_ticks)
{
    (void)client; (void)timeout_ticks;
    g_send_cont_msg_count++;
    return bin_record(0x0, false, data, len, len);
}

int mock_esp_websocket_client_send_fin(
    esp_websocket_client_handle_t client, int timeout_ticks)
{
    (void)client; (void)timeout_ticks;
    g_send_fin_count++;
    return bin_record(0x0, true, "", 0, 0);
}

void mock_esp_websocket_client_fail_at_index_set(int idx)
{
    g_bin_fail_at_index = idx;
}

size_t mock_esp_websocket_client_bin_op_call_count(void)
{
    return g_bin_op_count;
}

size_t mock_esp_websocket_client_send_bin_call_count(void)
{
    return g_send_bin_count;
}

size_t mock_esp_websocket_client_send_bin_partial_call_count(void)
{
    return g_send_bin_partial_count;
}

size_t mock_esp_websocket_client_send_cont_msg_call_count(void)
{
    return g_send_cont_msg_count;
}

size_t mock_esp_websocket_client_send_fin_call_count(void)
{
    return g_send_fin_count;
}

esp_err_t mock_esp_websocket_client_get_bin_frame_at(
    size_t idx, uint8_t *opcode, bool *fin,
    uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!out_len) return ESP_ERR_INVALID_ARG;
    if (idx >= s_bin_ring_total) { *out_len = 0; return ESP_ERR_NOT_FOUND; }
    /* idx 0 = oldest recorded; the ring holds the newest
     * MOCK_WS_BIN_RING_CAP frames in order. */
    size_t first_kept = (s_bin_ring_total > MOCK_WS_BIN_RING_CAP)
                          ? s_bin_ring_total - MOCK_WS_BIN_RING_CAP : 0;
    const bin_frame_slot_t *s = &s_bin_ring[idx - first_kept];
    if (opcode) *opcode = s->opcode;
    if (fin)    *fin    = s->fin;
    size_t n = s->len < out_cap ? s->len : out_cap;
    if (n > 0 && out) memcpy(out, s->data, n);
    *out_len = s->len;
    return (s->len <= out_cap) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
