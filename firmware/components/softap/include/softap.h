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

#ifdef __cplusplus
}
#endif
