/* softap.c — softAP provisioning bring-up (FW-05).
 *
 * The function `softap_run_provisioning(cfg)` performs the FR-1a
 * step 4 bring-up sequence:
 *   1. esp_wifi_init(&cfg_init)         — required first; without this
 *                                         esp_wifi_set_mode returns
 *                                         ESP_ERR_WIFI_NOT_INIT (caught
 *                                         on device flash, see
 *                                         engram #3627)
 *   2. esp_wifi_set_mode(WIFI_MODE_AP)
 *   3. esp_netif_create_default_wifi_ap()
 *   4. esp_wifi_set_config(WIFI_IF_AP, &cfg_ap) — SSID = ESP_<MAC>,
 *      WIFI_AUTH_OPEN (R-26 satisfied by FW-05.4 validation +
 *      FW-08 teardown)
 *   5. esp_wifi_start()
 *   6. httpd_start(&server, &cfg_httpd)
 *   7. httpd_register_uri_handler for /whoami (GET) + /provision (POST)
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
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"

/* Mock link headers — must come before the production headers that
 * declare the symbols being redirected. */
#ifdef UNITY_HOST_BUILD
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_esp_event_link.h"
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

/* Forward decls — implemented in softap_handlers.c and softap_home.c. */
extern esp_err_t whoami_get_handler_impl(httpd_req_t *req);
extern esp_err_t provision_post_handler_impl(httpd_req_t *req);
extern esp_err_t home_get_handler_impl(httpd_req_t *req);

static boot_status_t softap_bring_up(const config_t *cfg)
{
    boot_status_t fail = { .ret = ESP_FAIL, .step = BOOT_STEP_SOFTAP_START };

    /* Step 1: esp_netif_init — one-time underlying TCP/IP stack init.
     * Idempotent per IDF docs; safe to call repeatedly. Must come
     * before esp_netif_create_default_wifi_ap(). */
    esp_err_t r = esp_netif_init();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "softap_start failed: esp_netif_init: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }

    /* Step 2: default event loop — required for esp_wifi to deliver
     * WIFI_EVENT_AP_START etc. Idempotent (returns ESP_ERR_INVALID_STATE
     * if already created). */
    r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "softap_start failed: esp_event_loop_create_default: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }

    /* Step 3: create the AP netif BEFORE esp_wifi_init. IDF v5.5.3
     * requires this order — calling esp_netif_create_default_wifi_ap()
     * after esp_wifi_init returns ESP_ERR_INVALID_STATE because the
     * internal handler setup conflicts with the wifi driver state
     * (caught on device flash, engram #3630). */
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL) {
        ESP_LOGE(TAG, "softap_start failed: netif null");
        fail.ret = ESP_FAIL;
        return fail;
    }

    /* Step 4: esp_wifi_init — must be called before any other wifi
     * API. Without this, the device returns ESP_ERR_WIFI_NOT_INIT on
     * esp_wifi_set_mode (bug discovered via device flash on 2026-08-21,
     * engram #3627). We use IDF's WIFI_INIT_CONFIG_DEFAULT() macro on
     * device; the host mock ignores the cfg pointer. */
    wifi_init_config_t cfg_init = WIFI_INIT_CONFIG_DEFAULT();
    r = esp_wifi_init(&cfg_init);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: esp_wifi_init: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }
    ESP_LOGI(TAG, "esp_wifi_init ok");

    /* Step 5: mode */
    r = esp_wifi_set_mode(WIFI_MODE_AP);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }

    /* Step 6: config. Set max_connection explicitly to 4 — a memset(&cfg, 0)
     * sets max_connection=0 and the wifi driver rejects every client
     * with "max connection, deauth!" (caught on device interaction,
     * engram #3636). authmode=WIFI_AUTH_OPEN satisfies R-26; channel=1
     * is the canonical provisioning channel. SSID is left empty so
     * IDF auto-generates "ESP_<last-3-MAC>" (the boot log confirms
     * "wifi:mode : softAP (c8:f0:9e:9d:50:09)").
     *
     * Note: IDF v5.5.3 does NOT expose WIFI_AP_DEFAULT_CONFIG() as a
     * macro; the IDF example uses direct designated initializers. We
     * follow the example pattern verbatim. */
    wifi_config_t wifi_cfg = {
        .ap = {
            .ssid            = {0},
            .password        = {0},
            .ssid_len        = 0,
            .channel         = 1,
            .authmode        = WIFI_AUTH_OPEN,
            .ssid_hidden     = 0,
            .max_connection  = 4,
            .beacon_interval = 100,
        },
    };
    r = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }

    /* Step 7: Wi-Fi start. IDF does not expose esp_wifi_ap_start() in
     * v5.5.3 — esp_wifi_start() works for both AP and STA mode. */
    r = esp_wifi_start();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }

    ESP_LOGI(TAG, "softAP up");

    /* Step 8: httpd start. Use HTTPD_DEFAULT_CONFIG() macro instead of
     * memset(&cfg, 0) — zero-init sets max_uri_handlers=0 and the
     * httpd driver returns ESP_ERR_HTTPD_ALLOC_MEM (caught on device
     * flash, engram #3631). The default config provides sensible
     * defaults: max_uri_handlers=8, stack_size=4096, task_priority=5. */
    httpd_handle_t server = NULL;
    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
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

    /* Home page (FW-05 scope expansion, 2026-08-22): GET / serves a
     * minimal HTML form that POSTs to /provision. Without this, a
     * phone user connecting to the softAP has no way to issue the
     * POST. The PRD explicitly deferred captive-portal UX; this is
     * the smallest thing that ships the value. See softap_home.c. */
    httpd_uri_t home_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = home_get_handler_impl,
        .user_ctx = (void *)cfg,
    };
    r = httpd_register_uri_handler(server, &home_uri);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "softap_start failed: %s", esp_err_to_name(r));
        fail.ret = r;
        return fail;
    }
    ESP_LOGI(TAG, "URI / registered");

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
