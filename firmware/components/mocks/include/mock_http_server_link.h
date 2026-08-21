/* mock_http_server_link.h — macro-redirect for the http_server API.
 *
 * Mirrors mock_nvs_flash_link.h: every httpd_* call site is replaced
 * by the mock_* symbol below when MOCK_HTTP_USE_REAL is NOT defined.
 *
 * IMPORTANT: this header MUST be included AFTER the production
 * esp_http_server.h is included on device builds, but BEFORE on
 * host. The redirect macros apply regardless. The mock only runs on
 * the host (UNITY_HOST_BUILD defined); the production build links
 * the real esp_http_server component via REQUIRES in CMakeLists.
 *
 * Note: we deliberately do NOT alias httpd_req_t via the macros —
 * the host stub in mock_http_server.h already typedefs it to
 * mock_httpd_req_t so production source code can use it directly.
 */
#pragma once

#include "mock_http_server.h"

#ifndef MOCK_HTTP_USE_REAL

#define httpd_start(server, cfg)               mock_httpd_start(server, cfg)
#define httpd_stop(server)                     mock_httpd_stop(server)
#define httpd_register_uri_handler(server, u)  mock_httpd_register_uri_handler(server, u)
#define httpd_req_recv(req, buf, max)          mock_httpd_req_recv(req, buf, max)
#define httpd_resp_send(req, buf, len)         mock_httpd_resp_send(req, buf, len)
#define httpd_resp_set_type(req, type)         mock_httpd_resp_set_type(req, type)
#define httpd_resp_sendstr(req, str)           mock_httpd_resp_sendstr(req, str)

#endif  /* !MOCK_HTTP_USE_REAL */
