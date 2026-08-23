/* ws_text_frame.c — WS text-frame builder skeletons (FW-13, T-13-C).
 *
 * Four pure builders live here so they can be unit-tested without
 * any IDF runtime:
 *
 *   ws_text_frame_build_hello(id, out, out_len)        — T-13-E
 *   ws_text_frame_build_status(metrics, id, out, out_len) — T-13-H
 *   ws_url_build(out, out_len)                          — T-13-D
 *   ws_text_frame_parse_uri_path(uri, path_out, path_len) — T-13-D
 *
 * T-13-C returns 0 (empty) for the JSON builders; the URL builders
 * are no-ops. Real impls land in T-13-D/E/H/I.
 *
 * Convention: all builders return `size_t` (bytes written excluding
 * NUL). When the output buffer is too small, builders return 0
 * (zero is the "no frame emitted" sentinel — the caller can detect
 * via `len == 0` and skip the esp_websocket_client_send_text call).
 */
#include "ws.h"
#include "identity.h"

#include <string.h>

size_t ws_text_frame_build_hello(const device_identity_t *id,
                                  char *out, size_t out_len)
{
    (void)id;
    (void)out;
    (void)out_len;
    /* T-13-C skeleton: real JSON emitter lands in T-13-E GREEN. */
    return 0;
}

size_t ws_text_frame_build_status(const ws_runtime_metrics_t *m,
                                   const device_identity_t *id,
                                   char *out, size_t out_len)
{
    (void)m;
    (void)id;
    (void)out;
    (void)out_len;
    /* T-13-C skeleton: real JSON emitter lands in T-13-H GREEN. */
    return 0;
}

esp_err_t ws_url_build(char *out, size_t out_len)
{
    (void)out;
    (void)out_len;
    /* T-13-C skeleton: real URL composer + Pass-11 MAC-substring
     * guard land in T-13-D GREEN. */
    return ESP_OK;
}

esp_err_t ws_text_frame_parse_uri_path(const char *uri,
                                        char *path_out,
                                        size_t path_len)
{
    (void)uri;
    (void)path_out;
    (void)path_len;
    /* T-13-C skeleton: real parser lands in T-13-D GREEN. */
    return ESP_OK;
}