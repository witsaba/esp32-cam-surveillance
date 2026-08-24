/* softap_sta_listener.c — always-on /whoami on the STA interface (FW-05.5).
 *
 * Problem: the softAP HTTP server (FW-05) serves /whoami only while
 * the device is in provisioning mode. Once the STA interface gets an
 * IP, FR-1 step 4 tears down the softAP + its httpd — /whoami
 * becomes unreachable. Operators wanting to read a device's current
 * identity after provisioning have no way to do so without
 * re-entering provisioning mode.
 *
 * Solution: this module registers a SECOND httpd instance bound to
 * the STA interface. It subscribes to IP_EVENT_STA_GOT_IP via
 * wifi_event_subscribe (FW-08's wifi component is a separate
 * subscriber on the same event — see mock_esp_event.c multi-handler
 * fix). On IP-up, it starts an httpd with /whoami GET. On
 * disconnect, it stops the httpd. The /whoami handler is the SAME
 * whoami_get_handler_impl from softap_handlers.c — it reads the
 * live identity via config_load() on every request so re-provisioning
 * is visible without a restart.
 *
 * Lifecycle:
 *   boot.c: softap_sta_listener_install() is called once after
 *   wifi_init() returns. The listener:
 *     1. Subscribes WIFI_EVT_STA_GOT_IP and WIFI_EVT_STA_DISCONNECTED
 *     2. Does NOT start the httpd (waits for IP-up event)
 *   On IP-up event:
 *     3. httpd_start() with HTTPD_DEFAULT_CONFIG() (same pattern as
 *        softap.c — memset(0) would zero max_uri_handlers)
 *     4. httpd_register_uri_handler("/whoami", HTTP_GET,
 *        whoami_get_handler_impl, NULL — handler calls config_load
 *        on every request so user_ctx=NULL is safe)
 *     5. Logs "softap_sta: URI /whoami registered on STA"
 *   On disconnect event:
 *     6. httpd_stop() and clear handle
 *   On factory reset (boot-time long-press): the wifi station mode
 *   tears down → IP_EVENT_STA_LOST_IP → softAP re-enters
 *   provisioning → this listener is dormant (the softAP httpd
 *   takes over /whoami until the next IP-up).
 *
 * Security: the PRD's LAN-trust model applies. This endpoint is
 * unauthenticated by design (operator answered "LAN trust, no auth"
 * for the MVP). An attacker on the same wifi can enumerate devices
 * by scanning for /whoami responses — this is documented as a
 * known trade-off and may be hardened later via bearer-token auth.
 */
#include "softap.h"

#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"

#include "wifi_event.h"
#include "wifi.h"
#include "config.h"

#ifdef UNITY_HOST_BUILD
#include "mock_http_server_link.h"
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_system_link.h"
#endif

static const char *TAG = "softap_sta";

/* Module-static httpd handle. We track it explicitly (rather than
 * relying on the global g_current_server in mock_http_server.c)
 * because the softAP httpd is also module-static and the two httpd
 * instances must not be confused. */
static httpd_handle_t s_sta_httpd = NULL;

/* Forward decls — implemented in softap_handlers.c. */
extern esp_err_t whoami_get_handler_impl(httpd_req_t *req);
extern esp_err_t snapshot_get_handler_impl(httpd_req_t *req);

/* IP-up handler: start the STA-bound httpd and register /whoami.
 * Idempotent — if the httpd is already running (e.g., duplicate
 * IP-up event from a wifi reconnect), return without starting a
 * second instance. */
static void on_sta_got_ip_handler(void *arg,
                                  const char *event_base,
                                  int32_t event_id,
                                  void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    if (s_sta_httpd != NULL) {
        ESP_LOGD(TAG, "STA listener already running; ignoring duplicate IP-up");
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    esp_err_t r = httpd_start(&s_sta_httpd, &cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(r));
        s_sta_httpd = NULL;
        return;
    }

    /* user_ctx = NULL: the handler reads the live identity via
     * config_load() on every request. This means re-provisioning
     * changes are visible without a restart or any cache
     * invalidation. */
    httpd_uri_t whoami_uri = {
        .uri      = "/whoami",
        .method   = HTTP_GET,
        .handler  = whoami_get_handler_impl,
        .user_ctx = NULL,
    };
    r = httpd_register_uri_handler(s_sta_httpd, &whoami_uri);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "register /whoami failed: %s", esp_err_to_name(r));
        httpd_stop(s_sta_httpd);
        s_sta_httpd = NULL;
        return;
    }
    ESP_LOGI(TAG, "URI /whoami registered on STA interface");

    /* Diagnostic /snapshot — one real JPEG per GET, pulled from the
     * capture queue (single-caller invariant intact). Bisect tool:
     * 200 = camera alive; 503 no_frame = sensor/driver dead. */
    httpd_uri_t snapshot_uri = {
        .uri      = "/snapshot",
        .method   = HTTP_GET,
        .handler  = snapshot_get_handler_impl,
        .user_ctx = NULL,
    };
    r = httpd_register_uri_handler(s_sta_httpd, &snapshot_uri);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "register /snapshot failed: %s", esp_err_to_name(r));
        /* /whoami alone still has value; keep the listener up. */
        return;
    }
    ESP_LOGI(TAG, "URI /snapshot registered on STA interface");
}

/* Disconnect handler: stop the httpd. Idempotent — safe to call
 * when no httpd is active. */
static void on_sta_disconnected_handler(void *arg,
                                        const char *event_base,
                                        int32_t event_id,
                                        void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    if (s_sta_httpd == NULL) {
        return;
    }
    esp_err_t r = httpd_stop(s_sta_httpd);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "httpd_stop returned %s", esp_err_to_name(r));
    }
    s_sta_httpd = NULL;
    ESP_LOGI(TAG, "STA listener stopped");
}

/* Public API — called once from boot.c after wifi_init() returns. */
esp_err_t softap_sta_listener_install(void)
{
    esp_err_t r = wifi_event_subscribe(WIFI_EVT_STA_GOT_IP,
                                       on_sta_got_ip_handler, NULL);
    if (r != ESP_OK) return r;
    r = wifi_event_subscribe(WIFI_EVT_STA_DISCONNECTED,
                             on_sta_disconnected_handler, NULL);
    return r;
}

/* Test-only: query whether the listener is currently active. The
 * httpd handle is the source of truth. */
bool softap_sta_listener_is_active(void)
{
    return s_sta_httpd != NULL;
}

/* Test-only: reset module-static state between tests so the mock
 * surface stays clean. Mirrors the softap_stop() pattern. */
void softap_sta_listener_reset_for_test(void)
{
    if (s_sta_httpd != NULL) {
        httpd_stop(s_sta_httpd);
        s_sta_httpd = NULL;
    }
}
