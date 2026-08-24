/* stream_sender.c — FW-15 thin sender: maps the PURE fragment plan
 * onto the IDF esp_websocket_client binary-send verbs.
 *
 * Chunk size is read DIRECTLY from CONFIG_FIRMWARE_WS_BUFFER_SIZE
 * (design D3 — single knob; already mirrored as -D cflag in
 * run_host_tests.py for host builds).
 *
 * Mapping (REQ-ST-001/002/003):
 *
 *   n == 1   → esp_websocket_client_send_bin          (opcode 0x2)
 *   n > 1    → esp_websocket_client_send_bin_partial  (first, 0x2/FIN=0)
 *              esp_websocket_client_send_cont_msg*    (0x0/FIN=0)
 *              esp_websocket_client_send_fin          (terminator)
 *
 * send_fin carries no payload in v1.8.0, so the LAST PAYLOAD
 * slice rides a continuation message. Every slice is ≤ chunk,
 * so each call streams through the client's TX buffer intact.
 *
 * Failure convention mirrors IDF: return payload length on
 * success, -1 when any attempt returns < 0 (caller aborts the
 * remaining fragments — design D4 drain-drop-count).
 */
#include "stream.h"

#include <string.h>

#include "esp_log.h"

#include "stream_fragment.h"
#include "ws.h" /* ws_handle_get(); pulls the mock link headers on host */

#define TAG "stream"

/* Single knob (D3): the WS client's own TX buffer size. Each
 * fragment must fit it. */
#define STREAM_CHUNK CONFIG_FIRMWARE_WS_BUFFER_SIZE

/* Per-attempt timeout: the IDF network_timeout_ms bounds the wire
 * anyway; this bounds the internal queue handoff. On host the mock
 * ignores the value, so pass plain ms (no FreeRTOS ticks there). */
#ifdef UNITY_HOST_BUILD
#define STREAM_SEND_TICKS CONFIG_FIRMWARE_WS_NETWORK_TIMEOUT_MS
#else
#include "freertos/FreeRTOS.h"
#define STREAM_SEND_TICKS pdMS_TO_TICKS(CONFIG_FIRMWARE_WS_NETWORK_TIMEOUT_MS)
#endif

int stream_send_frame(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || len > (size_t)-1 / 2) return -1;

    esp_websocket_client_handle_t h = ws_handle_get();
    if (!h) {
        ESP_LOGW(TAG, "no ws handle — frame dropped");
        return -1;
    }

    size_t parts = stream_fragment_count(len, STREAM_CHUNK);

    /* Fast path (REQ-ST-001): fits in one message. */
    if (parts <= 1) {
        int rc = esp_websocket_client_send_bin(
            h, (const char *)buf, (int)len, STREAM_SEND_TICKS);
        return rc < 0 ? -1 : rc;
    }

    /* Fragmented path (REQ-ST-003): partial → cont* → fin. */
    int rc = esp_websocket_client_send_bin_partial(
        h, (const char *)buf, (int)STREAM_CHUNK, STREAM_SEND_TICKS);
    if (rc < 0) return -1;

    size_t off = STREAM_CHUNK;
    while (off < len) {
        size_t part = len - off;
        if (part > STREAM_CHUNK) part = STREAM_CHUNK;
        rc = esp_websocket_client_send_cont_msg(
            h, (const char *)(buf + off), (int)part, STREAM_SEND_TICKS);
        if (rc < 0) return -1;
        off += part;
    }

    /* Empty FIN terminator closes the fragmented message. */
    rc = esp_websocket_client_send_fin(h, STREAM_SEND_TICKS);
    return rc < 0 ? -1 : (int)len;
}
