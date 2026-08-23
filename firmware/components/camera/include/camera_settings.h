/* camera_settings.h — settings types + vtable for the FW-10.5 fake
 * source + FW-20.5 NVS-backed source swap.
 *
 * Two types live here:
 *
 *   camera_settings_t      — the 28 OV2640 sensor fields + a
 *                            schema_version stamp.
 *   camera_settings_source_t — function-pointer-table vtable
 *                            (`load`, `apply`, `reset_defaults`,
 *                             `schema_version`). The module-static
 *                            default in camera_settings.c is the
 *                            fake in-memory source; FW-20.5 swaps
 *                            it for the NVS-backed implementation
 *                            by calling camera_settings_set_source
 *                            _for_test on test paths + the real
 *                            setter on FW-20.5 production wiring.
 *
 * Shape mirrors the FW-08 wifi_event_subscribe registration seam
 * (`wifi.c:109-161`): a single swap point, mock-friendly via a
 * setter, zero overhead at the call site.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare sensor_t to keep this header IDF-free. The
 * production sensor_t is `typedef struct _sensor sensor_t;`
 * inside esp32-camera's sensor.h. The host mock mirrors that
 * typedef so production source compiles unchanged. */
struct _sensor;
typedef struct _sensor sensor_t;

/* Schema version stamp. The fake source returns 1; FW-20.5's
 * NVS-backed source will return 2 (or higher) when the layout
 * changes. The boot-time flow logs + ignores any blob with a
 * different schema_version. */
#ifndef CAMERA_SETTINGS_SCHEMA_VERSION
#define CAMERA_SETTINGS_SCHEMA_VERSION 1
#endif

/* The boot-time settings blob (28 OV2640 fields + schema_version).
 * FW-20.5 swaps the source backend that loads/stores this blob;
 * the field list stays the same. */
typedef struct {
    int16_t brightness;
    int16_t contrast;
    int16_t saturation;
    int16_t sharpness;
    int16_t denoise;
    int16_t special_effect;
    int16_t whitebal;
    int16_t awb_gain;
    int16_t wb_mode;
    int16_t aec;
    int16_t aec2;
    int16_t ae_level;
    int16_t aec_value;
    int16_t agc;
    int16_t agc_gain;
    int16_t gainceiling;
    int16_t bpc;
    int16_t wpc;
    int16_t raw_gma;
    int16_t lenc;
    int16_t hmirror;
    int16_t vflip;
    int16_t dcw;
    int16_t colorbar;
    int16_t framesize;
    int16_t quality;
    uint32_t schema_version;
} camera_settings_t;

/* The vtable. Each op is required (non-NULL) for a source. */
typedef struct {
    /* Load the stored blob into `out`. Returns ESP_OK on a hit,
     * ESP_ERR_NOT_FOUND when no blob is stored (caller uses
     * defaults), ESP_ERR_INVALID_VERSION on schema mismatch
     * (caller logs warning + falls back to defaults). */
    esp_err_t (*load)(camera_settings_t *out);

    /* Apply the blob to the live `sensor` via the sensor_t
     * setter surface. May be called multiple times — each
     * invocation is idempotent on the sensor. */
    esp_err_t (*apply)(sensor_t *sensor, const camera_settings_t *in);

    /* Reset `out` to the compiled-in defaults. Used at factory-
     * reset time and by the FW-20.5 NVS-erase-on-erase flow. */
    esp_err_t (*reset_defaults)(camera_settings_t *out);

    /* The schema version the source expects. Stale stored blobs
     * are detected by comparing source->schema_version() with
     * the blob's `schema_version` field. */
    uint32_t (*schema_version)(void);
} camera_settings_source_t;

/* Return the currently-installed source (defaults to
 * &fake_camera_settings_source). Never returns NULL. */
const camera_settings_source_t *camera_settings_get_source(void);

/* Test injection: replace the source pointer. Pass NULL to
 * restore the default. Production callers in FW-20.5 use this
 * to install the NVS-backed implementation at boot; tests use
 * it to install a stub source with a chosen schema_version +
 * stored blob. */
void camera_settings_set_source_for_test(const camera_settings_source_t *s);

/* Convenience helpers — the boot-time and runtime entry points
 * used by the camera component. */
esp_err_t camera_settings_apply(const camera_settings_t *in);
esp_err_t camera_settings_reset_defaults(camera_settings_t *out);

#ifdef __cplusplus
}
#endif
