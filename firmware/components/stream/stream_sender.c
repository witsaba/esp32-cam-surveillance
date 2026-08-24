/* stream_sender.c — FW-16 server-mode sender: pushes one complete
 * binary WS message per camera frame through the viewer sink.
 *
 * The device is a WebSocket SERVER (single inbound viewer on
 * CONFIG_FIRMWARE_WS_PATH). Frames travel via
 * httpd_ws_send_frame_async(hd, fd, pkt) with hd+fd captured at
 * handshake (see ws_server.c) — one pkt with .final = true per
 * camera_fb_t. QVGA JPEG frames (~3 KB) sit far below any
 * framing limit, so the FW-15 client-era fragment mapping
 * (send_bin_partial → cont* → fin) is gone; stream_fragment.c
 * stays as a pure diagnostic planner (parts= in the loop log).
 *
 * Failure convention preserved from FW-15 (design D4): return
 * payload length on success, -1 on any failure (no viewer
 * connected counts as failure). The caller returns the fb and
 * increments s_frames_dropped either way.
 */
#include "stream.h"

#include "esp_log.h"

#include "ws.h" /* sink seam; pulls the mock link headers on host */

#define TAG "stream"

int stream_send_frame(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || len > (size_t)-1 / 2) return -1;

    if (!ws_sink_connected()) {
        ESP_LOGW(TAG, "no viewer connected — frame dropped");
        return -1;
    }

    esp_err_t r = ws_sink_send_bin(buf, len);
    return (r == ESP_OK) ? (int)len : -1;
}
