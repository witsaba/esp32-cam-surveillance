/* esp_http_server.h — host stub of IDF's esp_http_server.h.
 *
 * The production source (softap.c, softap_handlers.c) calls
 * httpd_start, httpd_stop, httpd_register_uri_handler, httpd_req_recv,
 * httpd_resp_send, httpd_resp_set_type, httpd_resp_sendstr. Each
 * call site is redirected to the mock via mock_http_server_link.h
 * BEFORE this header is included.
 *
 * The full typedefs (httpd_config_t, httpd_uri_t, httpd_req_t via
 * mock_httpd_req_t, HTTP_GET, HTTP_POST) live in mock_http_server.h
 * and are visible when this header is included AFTER
 * mock_http_server_link.h (the production source's order).
 *
 * The mock's function prototypes are also in mock_http_server.h.
 * We do NOT re-declare them here — the link-header redirect would
 * turn any prototype below into a `mock_httpd_*` prototype that
 * would conflict with the real one. Instead we leave this header
 * essentially empty: production source can `#include "esp_http_server.h"`
 * and pick up the mock-provided typedefs + prototypes via the prior
 * link-header include.
 */
#ifndef HOST_ESP_HTTP_SERVER_H
#define HOST_ESP_HTTP_SERVER_H

/* Nothing to add — all types + prototypes live in mock_http_server.h
 * and are visible via the include-order contract. */
#include <sys/types.h>

#endif /* HOST_ESP_HTTP_SERVER_H */
