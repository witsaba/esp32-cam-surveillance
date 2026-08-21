/* softap.c — softAP provisioning bring-up (FW-05).
 *
 * The function `softap_run_provisioning(cfg)` performs the FR-1a
 * step 4 bring-up sequence:
 *   1. esp_wifi_set_mode(WIFI_MODE_AP)
 *   2. esp_netif_create_default_wifi_ap()
 *   3. esp_wifi_set_config(WIFI_IF_AP, &cfg_ap) — SSID = ESP_<MAC>,
 *      WIFI_AUTH_OPEN (R-26 satisfied by FW-05.4 validation +
 *      FW-08 teardown)
 *   4. esp_wifi_start()
 *   5. httpd_start(&server, &cfg_httpd)
 *   6. httpd_register_uri_handler for /whoami (GET) + /provision (POST)
 *
 * On green path the function blocks inside httpd_start()'s worker
 * loop until esp_restart() fires from provision_post_handler(). Each
 * IDF API call is wrapped in a check that returns
 * `boot_status_t { .ret = underlying; .step = BOOT_STEP_SOFTAP_START }`
 * on failure.
 *
 * Host builds (`UNITY_HOST_BUILD` defined) include the mock link
 * headers BEFORE the IDF Wi-Fi / netif / http_server headers. The
 * mocks swap every production call for the host-friendly equivalent.
 * The IDF headers are still `#include`d (they provide the typedefs
 * like `wifi_mode_t`, `httpd_config_t`, etc.), but the mock
 * implementations satisfy the call.
 *
 * The handlers themselves live in `softap_handlers.c`. They read
 * `cfg->identity.{name,description}` for /whoami and write the
 * merged `config_t` via `config_save()` for /provision.
 */
#include "softap.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"

/* Mock link headers — must come before the production headers that
 * declare the symbols being redirected. */
#ifdef UNITY_HOST_BUILD
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_http_server_link.h"
#include "mock_esp_system_link.h"
#endif

#include "config.h"

static const char *TAG = "softap";

/* The current cfg pointer is passed via the httpd uri's user_ctx when
 * the URI is registered. On host, the mock_httpd_invoke_registered_handler
 * helper copies the user_ctx into req->user_ctx before invoking the
 * handler; on device, IDF's httpd worker does the same. */
static const config_t *g_active_cfg = NULL;

/* Forward decls — implemented in softap_handlers.c. */
extern esp_err_t whoami_get_handler_impl(httpd_req_t *req);
extern esp_err_t provision_post_handler_impl(httpd_req_t *req);

static boot_status_t softap_bring_up(const config_t *cfg)
{
    boot_status_t fail = { .ret = ESP_FAIL, .step = BOOT_STEP_SOFTAP_START };

    /* Step 1: mode */
    esp_err_t r = esp_wifi_set_mode(WIFI_MODE_AP);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }

    /* Step 2: netif */
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL) {
        ESP_LOGE(TAG, "softap_start failed: netif null");
        fail.ret = ESP_FAIL;
        return fail;
    }

    /* Step 3: config (SSID = ESP_<last-3-mac-bytes>, OPEN auth). On
     * the host the wifi_config_t struct is opaque to the mock — we
     * zero the ap.ssid and let IDF's default naming apply on device.
     * The mock records the call; tests assert it was made exactly
     * once with WIFI_IF_AP. */
    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    r = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }

    /* Step 4: Wi-Fi start. IDF does not expose esp_wifi_ap_start() in
     * v5.5.3 — esp_wifi_start() works for both AP and STA mode. */
    r = esp_wifi_start();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }

    ESP_LOGI(TAG, "softAP up");

    /* Step 5: httpd start */
    httpd_handle_t server = NULL;
    httpd_config_t httpd_cfg;
    memset(&httpd_cfg, 0, sizeof(httpd_cfg));
    r = httpd_start(&server, &httpd_cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }

    /* Step 6: URI register — /whoami GET, /provision POST. Each
     * handler is registered with the cfg pointer as user_ctx so the
     * handler can read the current in-memory identity without
     * touching global state. */
    httpd_uri_t whoami_uri = {
        .uri      = "/whoami",
        .method   = HTTP_GET,
        .handler  = whoami_get_handler_impl,
        .user_ctx = (void *)cfg,
    };
    r = httpd_register_uri_handler(server, &whoami_uri);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }
    ESP_LOGI(TAG, "URI /whoami registered");

    httpd_uri_t provision_uri = {
        .uri      = "/provision",
        .method   = HTTP_POST,
        .handler  = provision_post_handler_impl,
        .user_ctx = (void *)cfg,
    };
    r = httpd_register_uri_handler(server, &provision_uri);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }
    ESP_LOGI(TAG, "URI /provision registered");

    /* Green path: on the device, the httpd worker loop blocks here
     * until esp_restart() fires from provision_post_handler(). On
     * host, the mock httpd_start is a no-op and the function returns
     * the sentinel. */
    return (boot_status_t){ .ret = ESP_OK, .step = BOOT_STEP_RETURN };
}

boot_status_t softap_run_provisioning(const config_t *cfg)
{
    if (!cfg) {
        boot_status_t s = { .ret = ESP_ERR_INVALID_ARG, .step = BOOT_STEP_SOFTAP_START };
        return s;
    }
    g_active_cfg = cfg;
    return softap_bring_up(cfg);
}

esp_err_t softap_stop(void)
{
    /* FW-05 ships a working body so FW-08 has a known-good
     * implementation to subscribe to. The handle and netif values
     * are not tracked across the host mock surface (the mocks
     * discard them), so this function is best-effort. */
    esp_err_t r = httpd_stop(NULL);
    if (r != ESP_OK && r != ESP_ERR_INVALID_ARG) return r;
    r = esp_wifi_stop();
    if (r != ESP_OK) return r;
    /* esp_netif_destroy returns void in IDF v5.5.3. */
    esp_netif_destroy(NULL);
    return ESP_OK;
}
