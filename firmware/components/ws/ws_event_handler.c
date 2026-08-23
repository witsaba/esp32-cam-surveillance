/* ws_event_handler.c — WS event handler registration skeleton
 * (FW-13, T-13-C GREEN-only).
 *
 * Three event handlers + one IP-up subscription:
 *   on_sta_got_ip       — fires esp_websocket_client_start (T-13-D)
 *   on_ws_connected     — emits hello + starts status timer (T-13-E)
 *   on_ws_disconnected  — stops status timer (T-13-H)
 *
 * T-13-C scope: install the IP_EVENT_STA_GOT_IP subscription via
 * the mock event registration (the mock captures the subscription
 * so a future test can verify it; today the subscription is a
 * no-op on host). Real CONNECTED/DISCONNECTED handler bodies land
 * in T-13-E + T-13-H.
 */
#include "ws.h"

#include <string.h>

#ifdef UNITY_HOST_BUILD
#include "mock_esp_event_link.h"
#include "mock_esp_websocket_client_link.h"
#include "wifi_event.h"
#include "wifi.h"
#include "esp_event.h"
#else
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "wifi_event.h"
#include "wifi.h"
#endif

/* No-op stub for IP-up handler — T-13-D GREEN replaces this with
 * the real `esp_websocket_client_start(s_ws_handle)` invocation. */
static void on_sta_got_ip(void *arg,
                           const char *event_base,
                           int32_t event_id,
                           void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    /* T-13-D: esp_websocket_client_start(s_ws_handle) goes here. */
}

/* No-op stub for WEBSOCKET_EVENT_CONNECTED handler — T-13-E GREEN
 * replaces with: identity_load + ws_text_frame_build_hello +
 * esp_websocket_client_send_text + esp_timer_start_periodic. */
static void on_ws_connected(esp_websocket_client_handle_t handle,
                             void *event_data)
{
    (void)handle;
    (void)event_data;
    /* T-13-E: real hello emit lands here. */
}

/* No-op stub for WEBSOCKET_EVENT_DISCONNECTED handler — T-13-H
 * GREEN replaces with: esp_timer_stop(g_status_timer) + log. */
static void on_ws_disconnected(esp_websocket_client_handle_t handle,
                                void *event_data)
{
    (void)handle;
    (void)event_data;
    /* T-13-H: real timer-stop + log lands here. */
}

/* Forward declarations of the IDF event-handler-instance symbols
 * the WS component uses. On host the mock_esp_event_link.h
 * redirects them to the in-memory capture table; on device the
 * real IDF esp_event_handler_instance_register is linked. */
esp_err_t ws_event_handler_install(void)
{
#ifdef UNITY_HOST_BUILD
    /* Register the IP-up subscription via the wifi component's
     * wifi_event_subscribe seam. T-13-D expands this to also
     * subscribe to the WEBSOCKET_EVENT_CONNECTED + DISCONNECTED
     * events via esp_websocket_register_events. */
    return wifi_event_subscribe(WIFI_EVT_STA_GOT_IP,
                                 (wifi_event_cb_t)on_sta_got_ip,
                                 NULL);
#else
    /* Device: same subscription; wifi_event_subscribe routes to
     * esp_event_handler_instance_register on the default loop. */
    return wifi_event_subscribe(WIFI_EVT_STA_GOT_IP,
                                 (wifi_event_cb_t)on_sta_got_ip,
                                 NULL);
#endif
}