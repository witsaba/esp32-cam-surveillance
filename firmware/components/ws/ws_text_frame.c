/* ws_text_frame.c — WS text-frame builders + URL helpers.
 * (FW-13; FW-16 server-mode cleanup.)
 *
 * Two pure builders + one URI parser live here so they can be
 * unit-tested without any IDF runtime:
 *
 *   ws_text_frame_build_hello(id, out, out_len)           — T-13-E GREEN
 *   ws_text_frame_build_status(metrics, id, out, out_len)  — T-13-I GREEN
 *   ws_text_frame_parse_uri_path(uri, path_out, path_len)   — T-13-D GREEN
 *
 * The client-era ws_url_build() composer was removed with the
 * outbound-client lifecycle (server mode registers a path
 * endpoint; there is no outbound URL to compose and
 * CONFIG_FIRMWARE_WS_URI_DEFAULT is gone).
 *
 * Convention: builders return `size_t` (bytes written excluding
 * NUL). When the output buffer is too small, builders return 0
 * (zero is the "no frame emitted" sentinel — the caller can detect
 * via `len == 0` and skip the send call).
 */
#include "ws.h"
#include "identity.h"

#include <string.h>
#include <stdio.h>

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

/* ---------- status builder (T-13-H GREEN) ----------
 *
 * Emits the status frame per REQ-WS-006 schema:
 *   {"type":"status","mac":"<12-hex>","name":"<nvs>",
 *    "uptime_s":<int>,"rssi_dbm":<int>,"free_heap":<int>,
 *    "fb_drops":<int>,"reconnects":<int>}
 *
 * Field order matches R-27 spec invariant for downstream
 * parsers. Empty name/description is allowed (unprovisioned
 * device; identity_load skips + logs).
 *
 * Returns: bytes written excluding NUL, or 0 if `out` is too
 * small / `m` or `id` is NULL. */
size_t ws_text_frame_build_status(const ws_runtime_metrics_t *m,
                                   const device_identity_t *id,
                                   char *out, size_t out_len)
{
    if (!m || !id || !out || out_len == 0) return 0;

    /* uptime_us → seconds (truncates — the cadence is per-30 s
     * so sub-second precision is meaningless to the backend). */
    int64_t uptime_s = m->uptime_us / 1000000;

    int n = snprintf(out, out_len,
        "{\"type\":\"status\","
         "\"mac\":\"%s\","
         "\"name\":\"%s\","
         "\"uptime_s\":%lld,"
         "\"rssi_dbm\":%d,"
         "\"free_heap\":%u,"
         "\"fb_drops\":%u,"
         "\"reconnects\":%u}",
        id->mac_hex,
        id->name,
        (long long)uptime_s,
        (int)m->rssi_dbm,
        (unsigned)m->free_heap,
        (unsigned)m->fb_drops,
        (unsigned)m->reconnects);
    if (n < 0 || (size_t)n >= out_len) return 0;
    return (size_t)n;
}
