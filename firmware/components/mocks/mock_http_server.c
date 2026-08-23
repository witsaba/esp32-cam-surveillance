/* mock_http_server.c — implementation of the IDF http_server mock.
 *
 * Stores up to 8 (uri, method, handler, user_ctx) tuples captured at
 * registration time. mock_httpd_invoke_registered_handler() walks the
 * array, finds the first (uri, method) match, and calls the handler
 * with the supplied mock_httpd_req_t cast to httpd_req_t *.
 *
 * mock_httpd_req_recv() drains mock_req->primed_recv_buffer into the
 * caller's buf, advancing an internal cursor. Returns the byte count
 * copied (0 when the cursor reaches the end).
 *
 * mock_httpd_resp_send() appends to mock_req->captured_response_buffer
 * (reallocating as needed). mock_httpd_resp_set_type() records the
 * Content-Type on the mock_req. mock_httpd_resp_sendstr() wraps
 * resp_send with strlen().
 */
#include "mock_http_server.h"

#include <stdlib.h>
#include <string.h>

#define MAX_HANDLERS 8

typedef struct {
    const char  *uri;
    int          method;
    esp_err_t  (*handler)(httpd_req_t *);
    void        *user_ctx;
    int          in_use;
} handler_entry_t;

static handler_entry_t g_handlers[MAX_HANDLERS];
static int             g_register_count = 0;
static int             g_start_count    = 0;
static int             g_stop_count     = 0;

static char g_sentinel_handle;
static httpd_handle_t g_current_server = &g_sentinel_handle;

void mock_httpd_reset(void)
{
    memset(g_handlers, 0, sizeof(g_handlers));
    g_register_count = 0;
    g_start_count    = 0;
    g_stop_count     = 0;
    g_current_server = &g_sentinel_handle;
}

int  mock_httpd_register_uri_handler_call_count(void) { return g_register_count; }
int  mock_httpd_registered_handler_count(void)       { return g_register_count; }
int  mock_httpd_start_call_count(void)                { return g_start_count; }
int  mock_httpd_stop_call_count(void)                 { return g_stop_count; }

void mock_httpd_last_registered_uri(const char **uri, int *method)
{
    /* Find the most recent registered handler. */
    for (int i = MAX_HANDLERS - 1; i >= 0; --i) {
        if (g_handlers[i].in_use) {
            if (uri)    *uri    = g_handlers[i].uri;
            if (method) *method = g_handlers[i].method;
            return;
        }
    }
    if (uri)    *uri    = NULL;
    if (method) *method = -1;
}

esp_err_t mock_httpd_invoke_registered_handler(const char *uri,
                                                int method,
                                                mock_httpd_req_t *req)
{
    if (!uri || !req) return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < MAX_HANDLERS; ++i) {
        if (!g_handlers[i].in_use) continue;
        if (g_handlers[i].method != method) continue;
        if (strcmp(g_handlers[i].uri, uri) != 0) continue;
        req->user_ctx = g_handlers[i].user_ctx;
        return g_handlers[i].handler((httpd_req_t *)req);
    }
    return ESP_ERR_NOT_FOUND;
}

void mock_httpd_req_set_primed_recv_buffer(mock_httpd_req_t *req,
                                            const char *body, size_t len)
{
    if (!req) return;
    if (req->primed_recv_buffer) {
        free(req->primed_recv_buffer);
        req->primed_recv_buffer = NULL;
    }
    if (body && len > 0) {
        req->primed_recv_buffer = (char *)malloc(len);
        if (req->primed_recv_buffer) {
            memcpy(req->primed_recv_buffer, body, len);
            req->primed_recv_len    = len;
            req->primed_recv_cursor = 0;
            req->content_len        = len;
        }
    }
}

void mock_httpd_req_set_user_ctx(mock_httpd_req_t *req, void *ctx)
{
    if (req) req->user_ctx = ctx;
}

mock_httpd_req_t *mock_httpd_req_new(void)
{
    mock_httpd_req_t *r = (mock_httpd_req_t *)calloc(1, sizeof(mock_httpd_req_t));
    return r;
}

void mock_httpd_req_free(mock_httpd_req_t *req)
{
    if (!req) return;
    if (req->primed_recv_buffer)     free(req->primed_recv_buffer);
    if (req->captured_response_buffer) free(req->captured_response_buffer);
    if (req->captured_content_type)    free(req->captured_content_type);
    free(req);
}

esp_err_t mock_httpd_start(httpd_handle_t *server, const httpd_config_t *cfg)
{
    (void)cfg;
    g_start_count++;
    if (server) *server = g_current_server;
    return ESP_OK;
}

esp_err_t mock_httpd_stop(httpd_handle_t server)
{
    (void)server;
    g_stop_count++;
    return ESP_OK;
}

esp_err_t mock_httpd_register_uri_handler(httpd_handle_t server,
                                            const httpd_uri_t *uri)
{
    (void)server;
    if (!uri || !uri->uri || !uri->handler) return ESP_ERR_INVALID_ARG;

    /* Find a free slot. */
    for (int i = 0; i < MAX_HANDLERS; ++i) {
        if (!g_handlers[i].in_use) {
            g_handlers[i].uri      = uri->uri;
            g_handlers[i].method   = (int)uri->method;
            g_handlers[i].handler  = uri->handler;
            g_handlers[i].user_ctx = uri->user_ctx;
            g_handlers[i].in_use   = 1;
            g_register_count++;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

int mock_httpd_req_recv(httpd_req_t *req, char *buf, size_t max)
{
    if (!req || !buf || max == 0) return 0;
    mock_httpd_req_t *m = (mock_httpd_req_t *)req;

    size_t remaining = (m->primed_recv_cursor < m->primed_recv_len)
        ? (m->primed_recv_len - m->primed_recv_cursor) : 0;
    if (remaining == 0) return 0;
    size_t copy = (remaining < max) ? remaining : max;
    memcpy(buf, m->primed_recv_buffer + m->primed_recv_cursor, copy);
    m->primed_recv_cursor += copy;
    return (int)copy;
}

esp_err_t mock_httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len)
{
    if (!req || !buf) return ESP_ERR_INVALID_ARG;
    mock_httpd_req_t *m = (mock_httpd_req_t *)req;

    size_t n = (len < 0) ? strlen(buf) : (size_t)len;
    size_t new_len = m->captured_response_len + n + 1;
    if (new_len > m->captured_response_cap) {
        size_t new_cap = (m->captured_response_cap == 0) ? 256 : m->captured_response_cap * 2;
        while (new_cap < new_len) new_cap *= 2;
        char *grown = (char *)realloc(m->captured_response_buffer, new_cap);
        if (!grown) return ESP_ERR_NO_MEM;
        m->captured_response_buffer = grown;
        m->captured_response_cap    = new_cap;
    }
    memcpy(m->captured_response_buffer + m->captured_response_len, buf, n);
    m->captured_response_len += n;
    m->captured_response_buffer[m->captured_response_len] = '\0';
    if (m->captured_status == 0) m->captured_status = 200;
    return ESP_OK;
}

esp_err_t mock_httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    if (!req || !type) return ESP_ERR_INVALID_ARG;
    mock_httpd_req_t *m = (mock_httpd_req_t *)req;
    if (m->captured_content_type) free(m->captured_content_type);
    m->captured_content_type = strdup(type);
    return m->captured_content_type ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mock_httpd_resp_sendstr(httpd_req_t *req, const char *str)
{
    return mock_httpd_resp_send(req, str, -1);
}
