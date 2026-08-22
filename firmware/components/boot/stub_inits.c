/* stub_inits.c — stubs for the remaining init interfaces FW-03 owns
 * as call-sites only.
 *
 * Each stub returns ESP_OK on the green path; on the host build
 * it consults `mock_init_returns_get(step)` first, so the
 * FW-03 ordering tests can force a non-OK return at any specific
 * step. The forced-return path short-circuits BEFORE the device
 * path (`#ifndef UNITY_HOST_BUILD`) so a failure mid-run does
 * not create the FreeRTOS task the stub would otherwise spin up.
 *
 * Each stub logs `// FW-NN: real impl lands in <milestone>` so
 * the next reviewer can grep for the follow-up. FW-08 replaced
 * `wifi_init` (the strong symbol now lives in firmware/components/
 * wifi/wifi.c — the linker resolves to that body). FW-10 replaces
 * `camera_init`, FW-13 replaces `ws_init` — when those land, the
 * orchestrator's call-site remains (one-line adaptations if
 * signatures differ).
 */
#include "boot.h"

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
#include "mock_init_returns.h"
#endif

static const char *TAG = "boot";

/* wifi_init moved to firmware/components/wifi/wifi.c — the strong
 * symbol resolves there via the linker. See T-08-A for the wifi
 * component skeleton commit. */

esp_err_t camera_init(const config_t *cfg) {
    (void)cfg;
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_CAMERA_INIT);
    if (forced != ESP_OK) return forced;
#endif
    ESP_LOGI(TAG, "stub: camera_init  // FW-10: real impl lands in camera driver init");
    return ESP_OK;
}

esp_err_t ws_init(const config_t *cfg) {
    (void)cfg;
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_WS_INIT);
    if (forced != ESP_OK) return forced;
#endif
    ESP_LOGI(TAG, "stub: ws_init  // FW-13: real impl lands in WS client init");
    return ESP_OK;
}