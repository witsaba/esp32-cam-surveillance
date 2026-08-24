/* ws.c — WS server-mode init implementation (FW-16).
 *
 * The device IS the WebSocket server (single inbound viewer on
 * CONFIG_FIRMWARE_WS_PATH). ws_init() therefore does NOT create
 * an outbound esp_websocket_client, does NOT wire lazy-start on
 * IP_EVENT_STA_GOT_IP, and has nothing for FW-14 to re-dial —
 * that wiring was removed with the client lifecycle (the retained
 * event-handler/backoff sources still compile; their isolated
 * suites self-wire a mock session).
 *
 * Production init sequence:
 *
 *   1. short-circuit honouring mock_init_returns_get(
 *      BOOT_STEP_WS_INIT) — the FW-03.2 bite-proof stays
 *      load-bearing.
 *   2. Install the disconnected sink stubs (a real sink is bound
 *      at /cams handshake accept — see ws_server.c).
 *   3. ws_status_timer_init() — creates the periodic 30 s handle
 *      (NOT started; start happens on viewer accept).
 *   4. Return ESP_OK.
 *
 * The boot.c call site `BOOT_CHECK_STEP(BOOT_STEP_WS_INIT,
 * ws_init(cfg))` is unchanged.
 */
#include "ws.h"
#include "ws_server.h"

#include <string.h>

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
#include "mock_init_returns.h"
#include "boot_status.h"
#else
#include "esp_websocket_client.h"
#endif

static const char *TAG = "ws";

/* Retained outbound-client handle. Always NULL in server mode
 * unless a retained-path consumer binds a session via
 * ws_handle_set() (isolated FW-13/FW-14 suites self-wire a mock;
 * production never calls it anymore). Single source of truth —
 * the event-handler TU reads it via ws_handle_get(). */
static esp_websocket_client_handle_t s_ws_handle = NULL;

void ws_handle_set(esp_websocket_client_handle_t h)
{
    s_ws_handle = h;
}

/* Built-in disconnected sink — the boot default and the state
 * restored whenever the viewer socket closes. Sends fail with
 * ESP_ERR_INVALID_STATE which maps onto the stream task's D4
 * drop-count path. */
static esp_err_t sink_disconnected_send_bin(const uint8_t *buf,
                                             size_t len)
{
    (void)buf;
    (void)len;
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t sink_disconnected_send_text(const char *buf,
                                              size_t len)
{
    (void)buf;
    (void)len;
    return ESP_ERR_INVALID_STATE;
}

static bool sink_disconnected_is_connected(void)
{
    return false;
}

static const ws_sink_t s_sink_disconnected = {
    .send_bin     = sink_disconnected_send_bin,
    .send_text    = sink_disconnected_send_text,
    .is_connected = sink_disconnected_is_connected,
};

/* Active sink. Never NULL after ws_init. Set by ws_sink_install
 * (device: ws_server.c at handshake; host tests: recorder). */
static const ws_sink_t *s_sink = &s_sink_disconnected;

void ws_sink_install(const ws_sink_t *vt)
{
    s_sink = (vt != NULL) ? vt : &s_sink_disconnected;
}

bool ws_sink_connected(void)
{
    return s_sink->is_connected();
}

esp_err_t ws_sink_send_bin(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_sink->is_connected()) return ESP_ERR_INVALID_STATE;
    return s_sink->send_bin(buf, len);
}

esp_err_t ws_sink_send_text(const char *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_sink->is_connected()) return ESP_ERR_INVALID_STATE;
    return s_sink->send_text(buf, len);
}

/* Honour the host-side forced-failure short-circuit so the
 * FW-03.2 bite-proof (`boot fails loud at ws_init when forced
 * non-OK`) stays load-bearing under this impl. */
static esp_err_t ws_init_short_circuit(void)
{
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_WS_INIT);
    if (forced != ESP_OK) return forced;
#endif
    return ESP_OK;
}

esp_err_t ws_init(const config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    esp_err_t r = ws_init_short_circuit();
    if (r != ESP_OK) return r;

    /* No viewer connected yet — install the disconnected stubs so
     * early frames from the stream task drop-count instead of
     * blocking or crashing. */
    ws_sink_install(NULL);

    /* Create the periodic 30 s status timer (NOT started — start
     * happens on /cams viewer accept in ws_server.c). */
    r = ws_status_timer_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "ws_init: ws_status_timer_init failed: %s",
                 esp_err_to_name(r));
        return r;
    }

    /* FW-16 — subscribe the /cams attach hook. The endpoint
     * itself lands when the softap STA listener's httpd comes up
     * (first IP_EVENT_STA_GOT_IP after listener start). Non-fatal
     * on failure: the device streams nothing but stays alive, and
     * the failure is loud in the log. */
    r = ws_server_install();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "ws_init: ws_server_install failed: %s",
                 esp_err_to_name(r));
        return r;
    }

    ESP_LOGI(TAG, "ws_init: ok mode=server path=%s "
                  "(single inbound viewer; endpoint attaches on "
                  "IP-up)",
             CONFIG_FIRMWARE_WS_PATH);
    return ESP_OK;
}

/* Retained accessor — NULL in server mode unless a retained-path
 * consumer bound a session (see ws_handle_set above). Kept so the
 * unwired FW-13/FW-14 sources and their isolated suites continue
 * to work unchanged. */
esp_websocket_client_handle_t ws_handle_get(void)
{
    return s_ws_handle;
}
