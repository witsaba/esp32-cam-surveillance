/* mock_http_server.h — host-side mock for the IDF HTTP server.
 *
 * The IDF `esp_http_server` component is heavyweight (worker task,
 * LwIP integration, etc.) and does not run on the host. This mock
 * captures every `httpd_register_uri_handler()` call into a static
 * `(uri, method, handler, user_ctx)` array, then lets tests invoke
 * the registered handler directly via
 * `mock_httpd_invoke_registered_handler(uri, method, mock_req)`.
 *
 * The mock also implements the request side:
 *   - `mock_httpd_req_t::primed_recv_buffer` is drained by
 *     `mock_httpd_req_recv()` (the POST body source).
 *   - `mock_httpd_req_t::captured_response_buffer` / `_status` /
 *     `captured_content_type` capture what the handler wrote.
 *
 * On host (UNITY_HOST_BUILD), the production source includes
 * `mock_http_server_link.h` which redirects `httpd_*` calls to the
 * mock symbols below. On host we ALSO alias `httpd_req_t` to
 * `mock_httpd_req_t` so the production handler signature
 * `esp_err_t (*)(httpd_req_t *)` matches at the call site.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Forward-declared type aliases matching IDF's httpd.h. On host
 * we only need `httpd_handle_t` (used by httpd_start/stop and
 * httpd_register_uri_handler). */
typedef void *httpd_handle_t;

/* The IDF httpd_config_t is large; on host we only need a subset.
 * The mock discards everything except existence (httpd_start just
 * stores a sentinel handle). The production source passes a
 * const `httpd_config_t *` to httpd_start — the mock ignores the
 * contents. */
typedef struct {
    int   task_priority;
    int   stack_size;
    int   core_id;
    int   server_port;
    int   ctrl_port;
    int   max_open_sockets;
    int   max_uri_handlers;
    int   max_resp_headers;
    int   backlog_conn;
    int   lru_purge_enable;
    int   recv_wait_timeout;
    int   send_wait_timeout;
    void *global_user_ctx;
    void *global_user_ctx_free;
    void *open_fn;
    void *close_fn;
    void *uri_match_fn;
} httpd_config_t;

/* The IDF httpd_req_t is opaque. On host we provide our own
 * mock-friendly struct that the registered handler sees. Forward-
 * declare the struct + typedef alias so httpd_uri_t can reference
 * httpd_req_t *. */
struct mock_httpd_req;
typedef struct mock_httpd_req httpd_req_t;

/* The IDF httpd_uri_t holds (uri, method, handler, user_ctx, ...).
 * Only these four are relevant on host. */
typedef int httpd_method_t;
#define HTTP_GET  0
#define HTTP_POST 1

typedef struct {
    const char  *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    void        *user_ctx;
} httpd_uri_t;

typedef struct mock_httpd_req {
    size_t content_len;
    char  *primed_recv_buffer;
    size_t primed_recv_len;
    size_t primed_recv_cursor;
    char  *captured_response_buffer;
    size_t captured_response_len;
    size_t captured_response_cap;
    int    captured_status;
    char  *captured_content_type;
    void  *user_ctx;
} mock_httpd_req_t;

/* ---------- handler dispatch registry ---------- */
void mock_httpd_reset(void);
int  mock_httpd_register_uri_handler_call_count(void);
void mock_httpd_last_registered_uri(const char **uri, int *method);
int  mock_httpd_registered_handler_count(void);

/* ---------- test entry point ---------- */
esp_err_t mock_httpd_invoke_registered_handler(const char *uri,
                                                int method,
                                                mock_httpd_req_t *req);

/* ---------- request helpers ---------- */
void mock_httpd_req_set_primed_recv_buffer(mock_httpd_req_t *req,
                                            const char *body, size_t len);
void mock_httpd_req_set_user_ctx(mock_httpd_req_t *req, void *ctx);
mock_httpd_req_t *mock_httpd_req_new(void);
void mock_httpd_req_free(mock_httpd_req_t *req);

/* ---------- mock targets (link-header redirects) ---------- */
esp_err_t mock_httpd_start(httpd_handle_t *server, const httpd_config_t *cfg);
esp_err_t mock_httpd_stop(httpd_handle_t server);
esp_err_t mock_httpd_register_uri_handler(httpd_handle_t server,
                                            const httpd_uri_t *uri);
int      mock_httpd_req_recv(httpd_req_t *req, char *buf, size_t max);
esp_err_t mock_httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len);
esp_err_t mock_httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t mock_httpd_resp_sendstr(httpd_req_t *req, const char *str);
