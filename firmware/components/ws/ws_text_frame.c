/* ws_text_frame.c — WS text-frame builders + URL helpers.
 * (FW-13, T-13-D GREEN partial; full impls land in T-13-E, T-13-H).
 *
 * Three pure builders + one URL composer live here so they can be
 * unit-tested without any IDF runtime:
 *
 *   ws_text_frame_build_hello(id, out, out_len)        — T-13-E GREEN
 *   ws_text_frame_build_status(metrics, id, out, out_len) — T-13-I GREEN
 *   ws_url_build(out, out_len)                          — T-13-D GREEN
 *   ws_text_frame_parse_uri_path(uri, path_out, path_len) — T-13-D GREEN
 *
 * Convention: builders return `size_t` (bytes written excluding
 * NUL). When the output buffer is too small, builders return 0
 * (zero is the "no frame emitted" sentinel — the caller can detect
 * via `len == 0` and skip the esp_websocket_client_send_text call).
 */
#include "ws.h"
#include "identity.h"

#include <string.h>
#include <stdio.h>

/* Compose the WS URI from CONFIG_FIRMWARE_WS_URI_DEFAULT +
 * CONFIG_FIRMWARE_WS_PATH. Both are string literals (mirrored via
 * -D cflags on host). The composition yields the literal
 * "ws://example.local:9000/cams" — the host the WS client
 * connects to per charter L1180 + PRD § FR-5. */
esp_err_t ws_url_build(char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;

    /* Snprintf-style bounded copy. The Kconfig values are
     * string literals; %s concatenation handles the join. The
     * CONFIG_FIRMWARE_WS_PATH value already starts with '/', so
     * we do not prepend another one (charter invariant: path is
     * exactly "/cams", never "ws://host/cams/" with a trailing
     * slash). */
    int n = snprintf(out, out_len, "%s%s",
                     CONFIG_FIRMWARE_WS_URI_DEFAULT,
                     CONFIG_FIRMWARE_WS_PATH);
    if (n < 0 || (size_t)n >= out_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

/* Parse the URI's path component into `path_out` (NUL-terminated
 * on success). Accepts the shape
 *   ws://host:port/path
 *   wss://host:port/path
 * The function scans for the third '/' (after scheme://host:port)
 * and copies everything from that point to the end-of-string
 * (excluding any trailing '?' query or '#' fragment — but the
 * FW-13 client config doesn't use those). */
esp_err_t ws_text_frame_parse_uri_path(const char *uri,
                                        char *path_out,
                                        size_t path_len)
{
    if (!uri || !path_out || path_len == 0) return ESP_ERR_INVALID_ARG;

    /* Locate the third '/' which marks the start of the path.
     *   scheme://host:port/path
     *   0123456789012345678901234
     * The first '/' is the scheme delimiter; the second '/' is
     * the host root. The third '/' opens the path. */
    int slash_count = 0;
    const char *p = uri;
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

    if (!path_start) {
        /* No path component (e.g. "ws://host:port" with no
         * trailing slash). Treat as empty path. */
        path_out[0] = '\0';
        return ESP_OK;
    }

    /* Find the end of the path (NUL, '?', or '#'). */
    const char *path_end = path_start;
    while (*path_end && *path_end != '?' && *path_end != '#') {
        path_end++;
    }

    size_t n = (size_t)(path_end - path_start);
    if (n + 1 > path_len) return ESP_ERR_INVALID_SIZE;

    memcpy(path_out, path_start, n);
    path_out[n] = '\0';
    return ESP_OK;
}

/* ---------- hello builder (T-13-E GREEN) ----------
 *
 * Emits the hello frame per REQ-WS-002 schema:
 *   {"type":"hello","mac":"<12-hex>","name":"<nvs>",
 *    "description":"<nvs>","fw":"<version>",
 *    "caps":["jpeg","stream","identify"]}
 *
 * Field order matches R-27 spec invariant for downstream parsers.
 * Empty name/description is allowed (unprovisioned device; the
 * ws init path logs ESP_LOGW from identity_load but still emits
 * the frame with empty fields).
 *
 * Returns: bytes written excluding NUL, or 0 if `out` is too
 * small / `id` is NULL. */
size_t ws_text_frame_build_hello(const device_identity_t *id,
                                  char *out, size_t out_len)
{
    if (!id || !out || out_len == 0) return 0;

    /* Hand-rolled emitter (no cJSON dep needed — the shape is
     * fixed and 6 fields, ~200 bytes worst case). fw field is
     * a compile-time string baked at build; today we emit
     * "1.0.0" (mirrors the FW-13 charter L1180 "1.0.0"). */
    int n = snprintf(out, out_len,
        "{\"type\":\"hello\","
         "\"mac\":\"%s\","
         "\"name\":\"%s\","
         "\"description\":\"%s\","
         "\"fw\":\"1.0.0\","
         "\"caps\":[\"jpeg\",\"stream\",\"identify\"]}",
        id->mac_hex,
        id->name,
        id->description);
    if (n < 0 || (size_t)n >= out_len) return 0;
    return (size_t)n;
}

/* T-13-I GREEN lands the full 8-field status JSON emitter
 * (uptime_s, rssi_dbm, free_heap, fb_drops, reconnects). Today
 * (T-13-G GREEN) we emit a minimal 3-field stub so the cadence
 * test asserts something. The full schema lands in T-13-H. */
size_t ws_text_frame_build_status(const ws_runtime_metrics_t *m,
                                   const device_identity_t *id,
                                   char *out, size_t out_len)
{
    if (!m || !id || !out || out_len == 0) return 0;
    int n = snprintf(out, out_len,
        "{\"type\":\"status\",\"mac\":\"%s\","
         "\"uptime_s\":%lld}",
        id->mac_hex,
        (long long)(m->uptime_us / 1000000));
    if (n < 0 || (size_t)n >= out_len) return 0;
    return (size_t)n;
}
