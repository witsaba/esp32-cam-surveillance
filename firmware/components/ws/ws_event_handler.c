/* ws_event_handler.c — WS event handler registration + bodies.
 * (FW-13, T-13-D GREEN).
 *
 * Three handlers + one installer:
 *   ws_event_handler_on_sta_got_ip        — fires
 *     esp_websocket_client_start (lazy start per FW-13.1).
 *   ws_event_handler_on_ws_connected      — emits hello + starts
 *     status timer (T-13-E + T-13-H GREEN land the bodies).
 *   ws_event_handler_on_ws_disconnected   — stops status timer
 *     (T-13-H GREEN).
 *   ws_event_handler_install()             — idempotent installer;
 *     wires the 3 handlers to their event sources.
 *
 * Today (T-13-D GREEN): the on_sta_got_ip body fires
 * esp_websocket_client_start; the CONNECTED/DISCONNECTED bodies
 * are stubs that T-13-E + T-13-H replace.
 *
 * On host: the wifi_event_subscribe surface routes to
 * mock_esp_event_handler_instance_register (mock_esp_event_link.h
 * redirect). The ws_event_handler_install() function is what
 * ws.c calls — keeps the cross-component surface thin.
 */
#include "ws.h"

#include <string.h>

#ifdef UNITY_HOST_BUILD
#include "mock_esp_event_link.h"
#include "mock_esp_websocket_client_link.h"
#include "mock_esp_timer_link.h"
#include "wifi_event.h"
#include "wifi.h"
#include "esp_event.h"
#else
#include "esp_event.h>
#include "esp_websocket_client.h>
#include "esp_timer.h"
#include "wifi_event.h"
#include "wifi.h"
#endif

#include "esp_log.h"

static const char *TAG = "ws_event";

/* Module-static state. The ws_handle is set by ws.c::ws_init_impl
 * (via ws_handle_set()) and read by the IP-up handler. */
static esp_websocket_client_handle_t s_ws_handle = NULL;

/* Idempotency: ws_event_handler_install() returns immediately if
 * already installed (mirrors the wifi component's idempotency
 * guard for backoff timer creation). The boot orchestrator calls
 * ws_init exactly once, but host tests reset mocks between tests
 * and may re-trigger the install path; the guard prevents
 * duplicate event-handler subscriptions from corrupting the
 * mock capture table. */
static bool s_event_handlers_installed = false;

/* Accessors used by ws.c to pass the handle to the event
 * handlers. ws_event_handler_install reads s_ws_handle and binds
 * the CONNECTED/DISCONNECTED subscriptions to it. */
void ws_handle_set(esp_websocket_client_handle_t h)
{
    s_ws_handle = h;
}

/* ---------- handler bodies ---------- */

/* IP_EVENT_STA_GOT_IP — fire esp_websocket_client_start (lazy
 * start). This is the only call to _start in production. The
 * mock fires CONNECTED synchronously inside _start, which then
 * chains into ws_event_handler_on_ws_connected (T-13-E GREEN
 * lands the hello emit). */
void ws_event_handler_on_sta_got_ip(void *arg,
                                     const char *event_base,
                                     int32_t event_id,
                                     void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    if (s_ws_handle == NULL) {
        ESP_LOGW(TAG, "sta_got_ip fired before ws_init set handle; "
                       "ignoring");
        return;
    }
    esp_err_t r = esp_websocket_client_start(s_ws_handle);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start failed: %s",
                 esp_err_to_name(r));
    }
}

/* WEBSOCKET_EVENT_CONNECTED — emits hello (T-13-E GREEN) + starts
 * status timer (T-13-H GREEN). The T-13-D GREEN body is a stub
 * that logs; subsequent leaves replace it. */
void ws_event_handler_on_ws_connected(void *handler_arg,
                                       esp_event_base_t base,
                                       int32_t event_id,
                                       void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)event_id;
    (void)event_data;
    /* T-13-E: identity_load + ws_text_frame_build_hello +
     * esp_websocket_client_send_text lands here. */
    /* T-13-H: ws_status_timer_start() lands here. */
    ESP_LOGI(TAG, "ws connected (hello + status-timer-start land in "
                  "T-13-E + T-13-H)");
}

/* WEBSOCKET_EVENT_DISCONNECTED — stops status timer (T-13-H
 * GREEN). The T-13-D GREEN body is a stub. */
void ws_event_handler_on_ws_disconnected(void *handler_arg,
                                          esp_event_base_t base,
                                          int32_t event_id,
                                          void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)event_id;
    (void)event_data;
    /* T-13-H: ws_status_timer_stop() lands here. */
    ESP_LOGI(TAG, "ws disconnected (status-timer-stop lands in T-13-H)");
}

/* ---------- installer ---------- */

/* Idempotent installer. Subscribes:
 *   1. WIFI_EVT_STA_GOT_IP   → ws_event_handler_on_sta_got_ip
 *   2. WEBSOCKET_EVENT_CONNECTED     → ws_event_handler_on_ws_connected
 *      WEBSOCKET_EVENT_DISCONNECTED  → ws_event_handler_on_ws_disconnected
 *
 * ws.c::ws_init_impl calls this after esp_websocket_client_init +
 * esp_websocket_register_events. On host the wifi subscription
 * captures the (base, id) tuple via mock_esp_event_handler_
 * instance_register; on device it goes through the IDF default
 * event loop. */
esp_err_t ws_event_handler_install(void)
{
    if (s_event_handlers_installed) return ESP_OK;

    esp_err_t r = wifi_event_subscribe(WIFI_EVT_STA_GOT_IP,
                                        (wifi_event_cb_t)
                                          ws_event_handler_on_sta_got_ip,
                                        NULL);
    if (r != ESP_OK) return r;

    /* Register CONNECTED + DISCONNECTED handlers on the WS
     * handle. ws.c calls this AFTER esp_websocket_register_events
     * (which subscribes the handlers to the WS event loop) — but
     * the mock captures per-handler registration independently
     * of the WS handle subscription, so we re-register here for
     * the host-side test surface (one subscription per (event_id,
     * cb) tuple). On device the esp_websocket_register_events
     * call inside ws.c is the actual subscription; this local
     * call is a no-op on device (the WS event loop is a private
     * loop, not the IDF default loop). */
    r = esp_websocket_register_events(s_ws_handle,
                                       WEBSOCKET_EVENT_CONNECTED,
                                       (esp_event_handler_t)
                                         ws_event_handler_on_ws_connected,
                                       NULL);
    if (r != ESP_OK) return r;

    r = esp_websocket_register_events(s_ws_handle,
                                       WEBSOCKET_EVENT_DISCONNECTED,
                                       (esp_event_handler_t)
                                         ws_event_handler_on_ws_disconnected,
                                       NULL);
    if (r != ESP_OK) return r;

    s_event_handlers_installed = true;
    return ESP_OK;
}
