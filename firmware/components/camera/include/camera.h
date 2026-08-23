/* camera.h — public API for the camera driver (FW-10).
 *
 * Single component owning the esp32-camera bring-up + sensor
 * setter reservation + camera_settings_source_t vtable seam
 * (FW-10.5). Mirrors firmware/components/wifi/wifi.h shape.
 *
 * Replaces the stub body at firmware/components/boot/stub_inits.c:
 * boot.c:152's call site is unchanged.
 *
 * Host builds (UNITY_HOST_BUILD defined) include the camera mock
 * link header BEFORE any esp32-camera symbol so the mocks take
 * over the IDF API surface.
 */
#pragma once

#include "boot_status.h"
#include "camera_settings.h"
#include "config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the esp32-camera driver on AI-Thinker with PRD § FR-2
 * parameters. Asserts PSRAM presence (FW-10.2); a typed-error log
 * `PSRAM_REQUIRED` is emitted on the missing-PSRAM path and
 * ESP_FAIL is returned without esp_restart(). On success: returns
 * ESP_OK after esp_camera_init() + the sensor_t accessor + the
 * Kconfig-default setters + the camera_settings_source->apply()
 * round trip.
 *
 * Must be called exactly once per boot — the no-reinit guard
 * (FW-10.3) trips TEST_FAIL_MESSAGE if a second invocation
 * appears in a single boot (the FW-10.3 bite-proof stub build).
 */
esp_err_t camera_init(const config_t *cfg);

/* Returns the cached sensor_t from the prior camera_init(). The
 * pointer is stable for the lifetime of the boot. Returns NULL
 * if camera_init() failed or has not been called. */
sensor_t *camera_sensor_get(void);

/* Runtime reconfig entry. Applies the framed `in` settings to
 * the cached sensor_t through its setter surface; NEVER calls
 * esp_camera_init again (FW-10.3 guard). */
esp_err_t camera_apply_runtime_settings(const camera_settings_t *in);

#ifdef __cplusplus
}
#endif
