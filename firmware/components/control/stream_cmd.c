/* stream_cmd.c — FW-19 `stream` command handler TU (design D3).
 *
 * One handler owns dispatch + acks + capture-gate wiring for the
 * stream command, running in the control-task context; every wire
 * write goes through ws_sink_send_text (TX-locked — the
 * control.c:270 reply seam, ruling 1's emission path). Ordering
 * pins (D3): START = ack THEN open the gate; STOP = clear the
 * gate THEN ack. Field validation precedes the state guard — a
 * malformed body reports bad_field regardless of viewer state
 * and never touches the gate. See stream_cmd.h for the full
 * wire contract. */
#include "stream_cmd.h"

#include <stdio.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"

#include "control.h"
#include "capture.h"
#include "ws.h"
#include "ws_server.h"

#define TAG "stream_cmd"

/* ---------- ruling-1 ack builder ---------- */

size_t stream_ok_build(bool on, uint32_t fps, char *out,
                       size_t out_len)
{
    if (!out || out_len == 0) return 0;

    int n = snprintf(out, out_len,
                     "{\"type\":\"stream_ok\",\"on\":%s,\"fps\":%u}",
                     on ? "true" : "false", (unsigned)fps);
    if (n < 0 || (size_t)n >= out_len) return 0; /* skip send */
    return (size_t)n;
}

/* Send-or-skip: 0-sentinel envelopes are silently dropped; sink
 * INVALID_STATE is the normal no-viewer state (control.c:270
 * precedent) — log-and-carry-on for anything else. */
static void stream_reply_send(const char *buf, size_t n)
{
    if (n == 0) return;
    esp_err_t r = ws_sink_send_text(buf, n);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "reply send failed: %s", esp_err_to_name(r));
    }
}

/* ---------- the registered handler ---------- */

static esp_err_t stream_cmd_handle(const char *raw_body, size_t len,
                                   void *ctx)
{
    (void)ctx;
    /* The router guarantees a strict-parsed allow-listed body
     * (FW-18.4 validate-before-setter); this branch is therefore
     * unreachable in production and stays silent by contract. */
    cJSON *doc = cJSON_ParseWithLength(raw_body, len);
    if (!doc) return ESP_ERR_INVALID_ARG;

    const cJSON *on  = cJSON_GetObjectItemCaseSensitive(doc, "on");
    const cJSON *fps = cJSON_GetObjectItemCaseSensitive(doc, "fps");
    const cJSON *id  = cJSON_GetObjectItemCaseSensitive(doc, "id");

    char buf[CONTROL_FRAME_MAX];

    /* 1) Field validation first — bad_field leaves state alone. */
    bool on_ok   = cJSON_IsBool(on);
    /* Absent fps is fine; present-but-not-integral-number is not. */
    bool fps_ok  = !fps || (cJSON_IsNumber(fps) &&
                            fps->valuedouble ==
                                (double)(long long)fps->valuedouble);

    if (!on_ok || !fps_ok) {
        size_t n = control_error_build("bad_field", id, buf,
                                       sizeof(buf));
        stream_reply_send(buf, n);
        cJSON_Delete(doc);
        return ESP_OK;
    }

    bool start = cJSON_IsTrue(on);

    /* 2) Pre-start viewer guard (D4/ruling 7): STOP passes. */
    if (start && !ws_server_viewer_active()) {
        size_t n = control_error_build("no_viewer", id, buf,
                                       sizeof(buf));
        stream_reply_send(buf, n);
        cJSON_Delete(doc);
        return ESP_OK;
    }

    if (start) {
        /* START: applied fps = clamped request or CONFIG default;
         * ack FIRST, then open the gate (D3 pin). */
        uint32_t applied =
            fps ? capture_fps_clamp((long long)fps->valuedouble)
                : (uint32_t)CONFIG_FIRMWARE_STREAM_FPS;

        size_t n = stream_ok_build(true, applied, buf, sizeof(buf));
        stream_reply_send(buf, n);
        capture_run_start(applied); /* clamps again — idempotent */
    } else {
        /* STOP: close the gate FIRST, then echo the CURRENT
         * applied fps (idempotent, viewerless-safe). */
        capture_run_stop();
        size_t n = stream_ok_build(false, capture_fps_get(), buf,
                                   sizeof(buf));
        stream_reply_send(buf, n);
    }

    cJSON_Delete(doc);
    return ESP_OK;
}

/* ---------- registration seam ---------- */

void stream_cmd_register(void)
{
    control_handler_register(CONTROL_CMD_STREAM, stream_cmd_handle);
}
