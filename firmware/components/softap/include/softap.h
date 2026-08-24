/* softap.h — public API for the softAP provisioning HTTP surface (FW-05).
 *
 * Owns:
 *   - softAP bring-up (esp_wifi_set_mode + esp_netif_create_default_wifi_ap
 *     + esp_wifi_set_config + esp_wifi_ap_start)
 *   - httpd_start + httpd_register_uri_handler for /whoami (GET) and
 *     /provision (POST)
 *   - JSON request parsing (cJSON) + strict validation
 *   - config_save() + esp_restart() on a well-formed /provision POST
 *
 * FW-08 will consume softap_stop() to tear down on STA-IP-up.
 *
 * Host tests use the macro-redirect link headers under
 * `firmware/components/mocks/include/<surface>_link.h` to swap each IDF API
 * call for a mock implementation. The component itself compiles
 * identically on host and device — only the link headers differ.
 */
#pragma once

#include "boot_status.h"
#include "config.h"
#include "esp_err.h"

#ifdef UNITY_HOST_BUILD
#include "mock_http_server_link.h"
#else
#include "esp_http_server.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Green-path blocking semantics. Returns ONLY on bring-up failure:
 *   - esp_wifi_set_mode != ESP_OK  → status { .ret = underlying; .step = BOOT_STEP_SOFTAP_START }
 *   - esp_netif_create_default_wifi_ap == NULL  → status { .ret = ESP_FAIL; .step = BOOT_STEP_SOFTAP_START }
 *   - esp_wifi_set_config != ESP_OK  → same
 *   - esp_wifi_ap_start != ESP_OK    → same
 *   - httpd_start != ESP_OK          → same
 *   - httpd_register_uri_handler != ESP_OK → same
 * On success, blocks until esp_restart() fires from provision_post_handler.
 * MUST NOT start FW-03 supervision tasks (provisioning branch invariant). */
boot_status_t softap_run_provisioning(const config_t *cfg);

/* Teardown. FW-08.4 will subscribe to STA-IP-up and call this. FW-05
 * ships a working body so FW-08 has a known-good implementation:
 *   httpd_stop(handle) + esp_wifi_stop() + esp_netif_destroy_default_netif(netif).
 * Returns ESP_OK on success; the failing esp_err_t on the first failure
 * (does NOT unwind partial state). */
esp_err_t softap_stop(void);

/* FW-08.5 — returns true while softap_run_provisioning() owns the
 * cfg and the softAP is still broadcasting. Returns false after
 * softap_stop() clears g_cfg_valid. Idempotent getter used by
 * the wifi component to decide whether to select WIFI_MODE_APSTA
 * at init time (so the captive portal stays alive during the
 * station-join window). On host, the link-header redirect routes
 * this to mock_softap_is_active_get(). */
bool softap_is_active(void);

/* FW-05.5 — install the always-on /whoami listener on the STA
 * interface. Called once from boot.c after wifi_init() returns.
 * Subscribes to IP_EVENT_STA_GOT_IP + IP_EVENT_STA_DISCONNECTED
 * via wifi_event_subscribe. The actual httpd start fires on the
 * first IP-up event (deferred — not all boots reach STA-IP-up
 * because the provisioning branch takes the softAP path).
 *
 * Idempotent: the first call subscribes the handlers; subsequent
 * calls (e.g., after a re-init) are a no-op. The FW-03 boot
 * orchestrator calls this exactly once per process. */
esp_err_t softap_sta_listener_install(void);

/* FW-05.5 — test entry. Returns true while the STA-bound httpd
 * is serving (i.e., the device is connected to a wifi network).
 * On host, the link-header redirects to the mock equivalent. */
/* FW-16 — live STA httpd handle (NULL while the listener is
 * down). The ws component reads this on IP-up to attach the /cams
 * WebSocket endpoint without a softap→ws link dependency. */
httpd_handle_t softap_sta_listener_httpd_handle_get(void);

bool softap_sta_listener_is_active(void);

/* FW-05.5 — test entry. Clears module-static state between
 * tests so the mock surface stays clean. Mirrors softap_stop()
 * semantics. */
void softap_sta_listener_reset_for_test(void);

#ifdef __cplusplus
}
#endif
