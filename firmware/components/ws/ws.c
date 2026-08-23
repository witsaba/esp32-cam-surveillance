/* ws.c — WS client init implementation (FW-13, T-13-D GREEN).
 *
 * Production init sequence (per design #3756 §4.1):
 *
 *   1. ws_url_build(s_ws_config.uri, sizeof(s_ws_config.uri))
 *      — compose "ws://CONFIG_FIRMWARE_WS_URI_DEFAULT host:port"
 *        + "/" + CONFIG_FIRMWARE_WS_PATH. The Pass-11 guard
 *        (T-13-F) lives here.
 *   2. s_ws_config.disable_auto_reconnect = true   (HARD INVARIANT —
 *      FW-14 owns reconnect; IDF built-in races the FW-14 counter).
 *   3. s_ws_handle = esp_websocket_client_init(&s_ws_config)
 *   4. esp_websocket_register_events(s_ws_handle,
 *        WEBSOCKET_EVENT_CONNECTED,    ws_on_ws_connected,    NULL)
 *      esp_websocket_register_events(s_ws_handle,
 *        WEBSOCKET_EVENT_DISCONNECTED, ws_on_ws_disconnected, NULL)
 *   5. wifi_event_subscribe(WIFI_EVT_STA_GOT_IP, ws_on_sta_got_ip, NULL)
 *      — Subscribes via the IDF default event loop. NO
 *        esp_websocket_client_start here (lazy start, fires from
 *        the IP-up handler).
 *   6. ws_status_timer_init()  — creates the periodic handle
 *      (NOT started).
 *   7. Return ESP_OK.
 *
 * The boot.c:153 call site is `BOOT_CHECK_STEP(BOOT_STEP_WS_INIT,
 * ws_init(cfg));` — the linker resolves this to stub_inits.c::ws_init
 * today (T-13-J rename). Both bodies honour the
 * mock_init_returns_get(BOOT_STEP_WS_INIT) short-circuit so the
 * FW-03.2 bite-proof stays load-bearing.
 *
 * Until T-13-J renames the symbol, this file's public entry is
 * named `ws_init_impl` (NOT `ws_init`) to avoid a duplicate-symbol
 * linker error against stub_inits.c::ws_init. The body itself is
 * the production init sequence; only the symbol name is shimmed.
 */
#include "ws.h"
#include "identity.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
#include "mock_init_returns.h"
#include "mock_esp_websocket_client_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_system_link.h"
#include "boot_status.h"
#include "wifi_event.h"
#include "wifi.h"
#include "esp_event.h"
#else
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "esp_system.h"
#include "wifi_event.h"
#include "wifi.h"
#endif

static const char *TAG = "ws";

/* Module-static config + handle. Set during ws_init_impl; read by
 * the event handlers (lazy start fires _start on IP-up; hello +
 * status frames emit on CONNECTED). Lifetime: process — the boot
 * orchestrator calls ws_init_impl exactly once. */
static esp_websocket_client_config_t s_ws_config;
static esp_websocket_client_handle_t  s_ws_handle = NULL;

/* Module-static URL buffer — sized to comfortably hold
 * CONFIG_FIRMWARE_WS_URI_DEFAULT + "/" + CONFIG_FIRMWARE_WS_PATH
 * + NUL. The IDF URI limit on host is 1024; we use 256 which
 * covers the default "ws://example.local:9000/cams" + ~150 bytes
 * of slack for runtime-overridable backend hostnames. */
#define WS_URI_BUF_LEN 256
static char s_uri_buffer[WS_URI_BUF_LEN];

/* Honour the host-side forced-failure short-circuit so the
 * FW-03.2 bite-proof (`boot fails loud at ws_init when forced
 * non-OK`) stays load-bearing under this real impl. */
static esp_err_t ws_init_short_circuit(void)
{
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_WS_INIT);
    if (forced != ESP_OK) return forced;
#endif
    return ESP_OK;
}

/* Build the WS URI from CONFIG_FIRMWARE_WS_URI_DEFAULT +
 * CONFIG_FIRMWARE_WS_PATH. The Kconfig symbols are string literals
 * (CONFIG_FIRMWARE_WS_URI_DEFAULT = "ws://example.local:9000";
 * CONFIG_FIRMWARE_WS_PATH = "/cams"). Concatenation yields
 * "ws://example.local:9000/cams". */
static esp_err_t ws_compose_uri(char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    /* Use the canonical ws_url_build helper from ws_text_frame.c
     * so the URI build has ONE source of truth (tested in T-13-D
     * RED via ws_text_frame_parse_uri_path; the compose helper is
     * its inverse and lands the same way). */
    return ws_url_build(out, out_len);
}

/* Production init body. Called by ws_init_impl (today) and
 * ws_init (post T-13-J rename). Returns ESP_OK on success. */
esp_err_t ws_init_impl(const config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    esp_err_t r = ws_init_short_circuit();
    if (r != ESP_OK) return r;

    /* Step 1: compose the URI. */
    r = ws_compose_uri(s_uri_buffer, sizeof(s_uri_buffer));
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "ws_init: ws_compose_uri failed: %s",
                 esp_err_to_name(r));
        return r;
    }

    /* Step 2-3: populate config + call esp_websocket_client_init.
     * The mock captures the config for host-test inspection
     * (mock_esp_websocket_client_get_last_uri, _get_transport,
     * _get_disable_auto_reconnect). */
    memset(&s_ws_config, 0, sizeof(s_ws_config));
    s_ws_config.uri                   = s_uri_buffer;
    s_ws_config.transport             = WEBSOCKET_TRANSPORT_OVER_TCP;
    s_ws_config.disable_auto_reconnect = true;  /* HARD INVARIANT */
    s_ws_config.buffer_size           = CONFIG_FIRMWARE_WS_BUFFER_SIZE;
    s_ws_config.ping_interval_sec     = CONFIG_FIRMWARE_WS_PING_INTERVAL_SEC;
    s_ws_config.pingpong_timeout_sec  = CONFIG_FIRMWARE_WS_PINGPONG_TIMEOUT_SEC;
    s_ws_config.network_timeout_ms    = CONFIG_FIRMWARE_WS_NETWORK_TIMEOUT_MS;
    s_ws_config.task_stack            = CONFIG_FIRMWARE_WS_TASK_STACK;

    s_ws_handle = esp_websocket_client_init(&s_ws_config);
    if (!s_ws_handle) {
        ESP_LOGE(TAG, "ws_init: esp_websocket_client_init returned NULL");
        return ESP_FAIL;
    }

    /* FW-13.4 — URL-no-MAC guard. The captured URI MUST NOT
     * contain the eFuse MAC substring anywhere. This is the
     * bite-proof for the charter invariant + the Pass-11 guard
     * trip. Production code never splices MAC into the URL
     * (CONFIG_FIRMWARE_WS_URI_DEFAULT + PATH are static
     * strings); the guard catches future bugs that would leak
     * identity into the URI.
     *
     * We check the URI as the mock captured it (mock may have
     * spliced MAC in via the Pass-11 gate). On device the mock
     * is absent and the URI is exactly what ws_url_build wrote.
     *
     * Read the MAC once (for the substring check) without
     * forcing a full identity_load — we only need the 12-hex
     * representation. */
    {
        const char *uri_captured = NULL;
#ifdef UNITY_HOST_BUILD
        uri_captured = mock_esp_websocket_client_get_last_uri();
#endif
        /* Fallback to s_ws_config.uri (the device path — host
         * builds without the mock wouldn't reach here, but the
         * conditional ensures the variable is referenced). */
        const char *uri_check = uri_captured ? uri_captured
                                              : s_ws_config.uri;
        uint8_t mac[6];
        char    mac_hex[13] = {0};
        if (uri_check &&
            esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK &&
            identity_mac_to_hex_lower(mac, mac_hex,
                                          sizeof(mac_hex)) == ESP_OK &&
            strstr(uri_check, mac_hex) != NULL) {
            ESP_LOGE(TAG, "url_no_mac invariant violated: "
                           "URI contains MAC substring (\"%s\" in %s)",
                           mac_hex, uri_check);
            return ESP_FAIL;
        }
    }

    /* Step 4 + 5: install the event handlers via ws_event_handler
     * _install(). This wires WEBSOCKET_EVENT_CONNECTED +
     * WEBSOCKET_EVENT_DISCONNECTED subscriptions to the WS
     * handle (via esp_websocket_register_events) and the
     * WIFI_EVT_STA_GOT_IP subscription via wifi_event_subscribe.
     * The handler bodies live in ws_event_handler.c. */
    ws_handle_set(s_ws_handle);
    r = ws_event_handler_install();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "ws_init: ws_event_handler_install failed: %s",
                 esp_err_to_name(r));
        return r;
    }

    /* Step 6: create the periodic 30 s status timer (NOT
     * started — start happens on WEBSOCKET_EVENT_CONNECTED in
     * T-13-H GREEN). */
    r = ws_status_timer_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "ws_init: ws_status_timer_init failed: %s",
                 esp_err_to_name(r));
        return r;
    }

    ESP_LOGI(TAG, "ws_init: ok uri=%s disable_auto_reconnect=true "
                  "transport=TCP (lazy start on IP_EVENT_STA_GOT_IP)",
             s_uri_buffer);
    return ESP_OK;
}

/* Touch the symbol so the linker keeps it until T-13-J renames
 * it to ws_init. Removed in the T-13-J commit. */
void ws_init_shim_dummy(void)
{
    (void)ws_init_impl;
}

/* Accessor for the lazy-start IP-up handler — exposed in ws.h
 * via ws_event_handler.h (forward declared). The IP-up handler
 * fires esp_websocket_client_start exactly once. Implemented
 * in ws_event_handler.c. */
esp_websocket_client_handle_t ws_handle_get(void)
{
    return s_ws_handle;
}
