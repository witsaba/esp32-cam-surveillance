/* ws_server.c — FW-16 device-as-server WebSocket endpoint.
 *
 * The product direction is INBOUND: the ESP32 listens, a viewer
 * connects to <CONFIG_FIRMWARE_WS_PATH> and receives the live
 * camera stream as binary WS frames. This module owns everything
 * session-shaped about that:
 *
 *   install     — subscribe IP_EVENT_STA_GOT_IP; when the softap
 *                 STA listener's httpd is live, register the WS
 *                 endpoint on it and capture the handle for async
 *                 sends (REUSES the existing server — no second
 *                 listener; the accessor avoids a softap→ws link
 *                 edge that would close the softap→ws→wifi→softap
 *                 dependency cycle).
 *   handshake   — on GET (the WS opening handshake): enforce the
 *                 single-viewer policy, capture hd+fd, install the
 *                 httpd-backed sink, emit hello, arm the 30 s
 *                 status timer.
 *   liveness    — the viewer slot is validated with
 *                 httpd_ws_get_fd_info() on every probe and every
 *                 failed send clears it (heal ≤ 1 frame period at
 *                 5 fps), so a vanished viewer frees the slot for
 *                 the next handshake without any server-wide
 *                 close-callback wiring.
 *
 * Frame delivery uses httpd_ws_send_frame_async(hd, fd, pkt) with
 * hd+fd captured at handshake. JPEG frames (~3 KB QVGA) ride ONE
 * complete binary frame each (pkt.final = true) — far below any
 * framing limit; no fragmentation layer exists in server mode
 * (stream_fragment.c stays as a pure diagnostic planner).
 *
 * Inbound TEXT frames carry camera commands (FW-18): the RX seam
 * copies each frame onto the bounded control ring for the
 * dispatcher task; non-TEXT frames are silently ignored. Every
 * error post-handshake is swallowed — the handler NEVER fails an
 * upgraded session (viewer state untouched).
 */
#include "ws_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "ws.h"
#include "control.h" /* control_frame_submit / drop record (RX seam) */
#include "identity.h"
#include "softap.h"
#include "wifi.h"       /* wifi_event_subscribe (GOT_IP attach hook) */
#include "wifi_event.h" /* WIFI_EVT_STA_GOT_IP */

#define TAG "ws_server"

/* Captured at endpoint registration so async sends work both for
 * the accepted viewer AND for rejection frames on a second
 * handshake. */
static httpd_handle_t s_httpd       = NULL;
static bool            s_registered = false;

/* The one active viewer session socket. -1 when idle. */
static int s_viewer_fd = -1;

/* Rejection body for a second concurrent viewer. Kept tiny and
 * greppable; the server closes the socket right after (ESP_FAIL). */
static const char VIEWER_LIMIT_JSON[] =
    "{\"type\":\"error\",\"reason\":\"viewer_limit\"}";

/* ---------- session liveness ---------- */

#ifdef UNITY_HOST_BUILD
static httpd_ws_client_info_t probe_fd_info(void)
{
    /* Host mock: the session is alive unless a test killed it via
     * mock_httpd_ws_kill_session(). */
    return mock_httpd_ws_session_alive(s_viewer_fd);
}
#else
static httpd_ws_client_info_t probe_fd_info(void)
{
    if (!s_httpd || s_viewer_fd < 0) return HTTPD_WS_CLIENT_INVALID;
    return httpd_ws_get_fd_info(s_httpd, s_viewer_fd);
}
#endif

/* Free the viewer slot + disarm the status cadence. Called from
 * the dead-send path and available to tests. */
static void viewer_clear(const char *reason)
{
    int fd = s_viewer_fd;
    s_viewer_fd = -1;
    ws_sink_install(NULL);
    esp_err_t r = ws_status_timer_stop();
    if (r != ESP_OK && r != ESP_ERR_INVALID_ARG &&
        r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "status timer stop failed: %s",
                 esp_err_to_name(r));
    }
    ESP_LOGI(TAG, "viewer disconnected fd=%d (%s) — slot freed",
             fd, reason);
}

/* ---------- httpd-backed sink (device implementation) ---------- */

static esp_err_t server_sink_send_bin(const uint8_t *buf, size_t len)
{
    if (!s_httpd || s_viewer_fd < 0 || !buf || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    httpd_ws_frame_t pkt = {
        .final   = true,
        .type    = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)buf,
        .len     = len,
    };
    esp_err_t r = httpd_ws_send_frame_async(s_httpd, s_viewer_fd, &pkt);
    if (r != ESP_OK) viewer_clear("send failed");
    return r;
}

static esp_err_t server_sink_send_text(const char *buf, size_t len)
{
    if (!s_httpd || s_viewer_fd < 0 || !buf || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    httpd_ws_frame_t pkt = {
        .final   = true,
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len     = len,
    };
    esp_err_t r = httpd_ws_send_frame_async(s_httpd, s_viewer_fd, &pkt);
    if (r != ESP_OK) viewer_clear("send failed");
    return r;
}

static bool server_sink_is_connected(void)
{
    return probe_fd_info() == HTTPD_WS_CLIENT_WEBSOCKET;
}

static const ws_sink_t s_server_sink = {
    .send_bin     = server_sink_send_bin,
    .send_text    = server_sink_send_text,
    .is_connected = server_sink_is_connected,
};

bool ws_server_viewer_active(void)
{
    return server_sink_is_connected();
}

/* ---------- handshake ---------- */

/* Accept THE viewer: capture fd, install the sink, emit hello,
 * arm the periodic status timer. */
static esp_err_t viewer_accept(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        ESP_LOGE(TAG, "handshake: httpd_req_to_sockfd failed");
        return ESP_FAIL;
    }

    s_viewer_fd = fd;
    ws_sink_install(&s_server_sink);

    /* Hello MUST be the first text frame of the session
     * (REQ-WS-002 shape preserved from the client era). */
    device_identity_t id;
    esp_err_t r = identity_load(&id);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "viewer accept: identity_load failed: %s",
                 esp_err_to_name(r));
        goto accept_done;
    }

    char buf[256];
    size_t len = ws_text_frame_build_hello(&id, buf, sizeof(buf));
    if (len == 0) {
        ESP_LOGE(TAG, "viewer accept: hello build returned 0");
        goto accept_done;
    }
    if (ws_sink_send_text(buf, len) != ESP_OK) {
        ESP_LOGE(TAG, "viewer accept: hello send failed");
    } else {
        ESP_LOGI(TAG, "hello sent to viewer fd=%d (mac=%s name=%s)",
                 fd, id.mac_hex, id.name);
    }

accept_done:
    /* Arm the 30 s status cadence even if the hello failed — the
     * timer cb skips silently while the sink is down. */
    r = ws_status_timer_start();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "viewer accept: status timer start failed: %s",
                 esp_err_to_name(r));
    }
    ESP_LOGI(TAG, "viewer accepted fd=%d (single-viewer slot taken)",
             fd);
    return ESP_OK;
}

/* Reject an additional viewer: short text error frame, then
 * ESP_FAIL so the httpd closes THAT socket. The active viewer's
 * session is untouched. */
static esp_err_t viewer_reject(int fd)
{
    ESP_LOGW(TAG, "second WS handshake fd=%d rejected "
                  "(single-viewer policy)", fd);
    httpd_ws_frame_t pkt = {
        .final   = true,
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)VIEWER_LIMIT_JSON,
        .len     = sizeof(VIEWER_LIMIT_JSON) - 1,
    };
    (void)httpd_ws_send_frame_async(s_httpd, fd, &pkt);
    return ESP_FAIL;
}

/* ---------- FW-18 command ingest (RX seam) ---------- */

/* Drain-discard `total` payload bytes off the socket in bounded
 * stack chunks so the WS stream stays frame-aligned. Used for
 * every frame the dispatcher does NOT keep (non-TEXT traffic,
 * oversize commands). */
static void ingest_drain_discard(httpd_req_t *req, size_t total)
{
    uint8_t scratch[64];
    httpd_ws_frame_t pkt = {0};
    pkt.type             = HTTPD_WS_TYPE_TEXT;
    size_t remaining     = total;
    while (remaining > 0) {
        pkt.payload = scratch;
        pkt.len     = (remaining < sizeof(scratch))
                          ? remaining : sizeof(scratch);
        if (httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK) {
            break;
        }
        remaining -= pkt.len;
    }
}

/* Post-handshake TEXT frame → heap copy → control ring. IDF
 * invokes the URI handler once per received WS frame; the recv
 * runs in two calls (size probe len=0, then fill). Frames the
 * dispatcher does not keep are chunk-drained and discarded so the
 * socket stream stays synced. ANY failure path logs and returns
 * ESP_OK — the request NEVER fails post-handshake. */
static esp_err_t ws_server_ingest_frame(httpd_req_t *req)
{
    httpd_ws_frame_t pkt = {0};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    if (httpd_ws_recv_frame(req, &pkt, 0) != ESP_OK) {
        ESP_LOGW(TAG, "cmd ingest: frame probe failed");
        return ESP_OK;
    }
    if (pkt.type != HTTPD_WS_TYPE_TEXT || pkt.len == 0) {
        /* PING/PONG/CLOSE/BINARY/empty: silent ignore (ruling
         * #3966.3) — no control-module state change; the payload
         * is drained so the stream keeps working. */
        ingest_drain_discard(req, pkt.len);
        return ESP_OK;
    }
    if (pkt.len > CONTROL_FRAME_MAX) {
        /* Oversize command: same drop counter as queue-full
         * (D3), DISTINCT log text, stream kept synced. */
        ingest_drain_discard(req, pkt.len);
        control_dropped_frame_record();
        ESP_LOGW(TAG, "cmd ingest: oversize %u B frame dropped "
                      "(max %u)", (unsigned)pkt.len,
                 (unsigned)CONTROL_FRAME_MAX);
        return ESP_OK;
    }

    char *copy = (char *)malloc((size_t)pkt.len + 1); /* +1 NUL */
    if (!copy) {
        ESP_LOGE(TAG, "cmd ingest: OOM — %u B frame dropped",
                 (unsigned)pkt.len);
        return ESP_OK;
    }
    pkt.payload = (uint8_t *)copy;
    if (httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK) {
        free(copy);
        ESP_LOGW(TAG, "cmd ingest: fill failed — frame dropped");
        return ESP_OK;
    }
    copy[pkt.len] = '\0';
    if (!control_frame_submit(copy)) {
        /* Ring full → drop NEWEST (ruling #3966.2): counter was
         * bumped inside the ring op, free NOW, nothing on wire. */
        free(copy);
        ESP_LOGW(TAG, "cmd ingest: control ring full — newest "
                      "command dropped");
    }
    return ESP_OK;
}

static esp_err_t ws_server_handler(httpd_req_t *req)
{
    if ((int)req->method == (int)HTTP_GET) {
        /* Opening handshake (IDF completed the 101 upgrade before
         * invoking us). */
        int fd = httpd_req_to_sockfd(req);
        if (ws_server_viewer_active() && fd != s_viewer_fd) {
            return viewer_reject(fd);
        }
        return viewer_accept(req);
    }
    /* Post-handshake inbound frames: TEXT commands are copied onto
     * the bounded control ring for the dispatcher task (FW-18 RX
     * seam); non-TEXT frames are silently ignored. Never fails
     * post-handshake. */
    return ws_server_ingest_frame(req);
}

/* ---------- registration ---------- */

esp_err_t ws_server_register(httpd_handle_t hd)
{
    if (!hd) return ESP_ERR_INVALID_ARG;

    httpd_uri_t cams_uri = {
        .uri          = CONFIG_FIRMWARE_WS_PATH,
        .method       = HTTP_GET,
        .handler      = ws_server_handler,
        .user_ctx     = NULL,
        .is_websocket = true,
    };
    return httpd_register_uri_handler(hd, &cams_uri);
}

/* IP_EVENT_STA_GOT_IP — attach the /cams endpoint onto the live
 * STA httpd instance. Idempotent: a duplicate event or a second
 * call after a successful registration is a no-op. If our
 * subscriber fires before the listener started the httpd (NULL
 * handle), the next GOT_IP retries. */
static void ws_server_on_got_ip(void *arg,
                                const char *event_base,
                                int32_t event_id,
                                void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    if (s_registered) return;

    httpd_handle_t hd = softap_sta_listener_httpd_handle_get();
    if (!hd) {
        ESP_LOGD(TAG, "IP-up: STA httpd not up yet; /cams attach "
                       "deferred");
        return;
    }

    s_httpd = hd;
    esp_err_t r = ws_server_register(hd);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s",
                 CONFIG_FIRMWARE_WS_PATH, esp_err_to_name(r));
        s_httpd = NULL;
        return;
    }
    s_registered = true;
    ESP_LOGI(TAG, "%s (WebSocket, single viewer) attached to "
                  "STA httpd", CONFIG_FIRMWARE_WS_PATH);
}

esp_err_t ws_server_install(void)
{
    return wifi_event_subscribe(WIFI_EVT_STA_GOT_IP,
                                ws_server_on_got_ip, NULL);
}

void ws_server_reset_for_test(void)
{
    s_httpd       = NULL;
    s_registered  = false;
    s_viewer_fd   = -1;
    ws_sink_install(NULL);
}
