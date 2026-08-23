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
 * wifi/wifi.c — the linker resolves to that body). FW-10 replaced
 * `camera_init` (firmware/components/camera/camera.c). FW-13
 * replaces `ws_init` — when that lands, the orchestrator's
 * call-site remains (one-line adaptations if signatures differ).
 */
#include "boot.h"
#include "ws.h"

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
#include "mock_init_returns.h"
#endif

static const char *TAG = "boot";

/* wifi_init moved to firmware/components/wifi/wifi.c — the strong
 * symbol resolves there via the linker. See T-08-A for the wifi
 * component skeleton commit. */

/* camera_init moved to firmware/components/camera/camera.c (FW-10) —
 * the strong symbol resolves there via the linker. */

/* ws_init — strong symbol consumed by boot.c:153. Honours the
 * mock_init_returns_get short-circuit (FW-03.2 bite-proof), then
 * dispatches to the production init body in firmware/components/
 * ws/ws.c::ws_init_impl. T-13-J renames ws_init_impl → ws_init
 * and deletes this body (one atomic step); today the bridge keeps
 * both the stub's short-circuit behaviour AND the real init body
 * load-bearing for host tests. */
esp_err_t ws_init(const config_t *cfg) {
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_WS_INIT);
    if (forced != ESP_OK) return forced;
#endif
    return ws_init_impl(cfg);
}