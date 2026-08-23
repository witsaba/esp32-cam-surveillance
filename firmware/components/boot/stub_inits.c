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
 * FW-08 replaced `wifi_init` (firmware/components/wifi/wifi.c).
 * FW-10 replaced `camera_init` (firmware/components/camera/camera.c).
 * FW-13 T-13-I replaced `ws_init` (firmware/components/ws/ws.c).
 * No strong symbols remain in this file — every init step
 * resolves to its production component via the linker. The
 * stub file persists to host the `// FW-NN: real impl lands
 * in <milestone>` audit log (no remaining lines) and for future
 * FW-NN init interfaces that may need a temporary bridge before
 * the production component lands.
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

/* camera_init moved to firmware/components/camera/camera.c (FW-10) —
 * the strong symbol resolves there via the linker. */

/* ws_init moved to firmware/components/ws/ws.c (FW-13 T-13-I) —
 * the strong symbol resolves there via the linker. T-13-I
 * atomic rename: ws_init_impl → ws_init + delete the dispatch
 * bridge here. The mock_init_returns_get short-circuit for
 * BOOT_STEP_WS_INIT now lives inside ws.c::ws_init. */