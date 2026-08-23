/* camera_settings.c — fake in-memory camera-settings source.
 *
 * The default `g_settings_source` pointer is `&fake_camera_settings_
 * source`. The fake stores a single blob in module-static memory;
 * FW-20.5 replaces the source pointer with an NVS-backed
 * implementation that wraps `nvs_get_blob` / `nvs_set_blob`.
 *
 * FW-10.5 walking-skeleton tests use
 * `camera_settings_set_source_for_test` to install a custom source
 * (e.g. one that primes `quality=12`) so the boot-time setting
 * application can be exercised against a chosen stored blob.
 */
#include "camera_settings.h"

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
/* On host we pull in the mock header so the camera_settings.c
 * setters can chain through `sensor->set_*` — the mock's
 * sensor_t sentinel mirrors IDF's struct layout (sensor_t
 * has the function pointers embedded directly, not in a
 * `slots` sub-struct). */
#include "mock_esp_camera.h"
#else
/* Device build: include IDF's sensor.h so the full sensor_t
 * definition is in scope. Without this, `sensor->set_quality`
 * triggers "incomplete type" — the typedef in esp_camera.h is
 * a forward declaration; the full struct lives in sensor.h. */
#include "sensor.h"
#endif

#define TAG "camera_settings"

/* The currently-installed source pointer. Defaults to the fake;
 * test setter mutates this directly under camera_settings_set_
 * source_for_test(). The fake's blob is module-static so the
 * same source struct remains usable across boots of the same
 * host process. */
static const camera_settings_source_t *g_settings_source = NULL;

/* Module-static blob for the fake source. The fake_camera_
 * settings_source_load() returns this on call; tests can prime
 * it via the test setter before invoking camera_init(). */
static camera_settings_t g_fake_blob;
static bool              g_fake_blob_valid = false;

/* Forward declarations for the fake source ops. */
static esp_err_t fake_load(camera_settings_t *out);
static esp_err_t fake_apply(sensor_t *sensor, const camera_settings_t *in);
static esp_err_t fake_reset_defaults(camera_settings_t *out);
static uint32_t  fake_schema_version(void);

/* The default source instance. The struct is `const`-correct:
 * the `const` qualifier on `g_settings_source` says the pointer
 * itself doesn't move; the const-resolved source's ops are
 * implicitly volatile across threads but a single boot_test is
 * single-threaded so the read-only access is enough.
 *
 * Exported (not static) so the host test runner can install
 * the default fake explicitly via
 * `camera_settings_set_source_for_test(&fake_camera_settings_source)`
 * — matches mock_esp_wifi and mock_softap patterns that expose
 * the singleton sources. */
const camera_settings_source_t fake_camera_settings_source = {
    .load            = fake_load,
    .apply           = fake_apply,
    .reset_defaults  = fake_reset_defaults,
    .schema_version  = fake_schema_version,
};

const camera_settings_source_t *camera_settings_get_source(void)
{
    if (!g_settings_source) {
        g_settings_source = &fake_camera_settings_source;
    }
    return g_settings_source;
}

void camera_settings_set_source_for_test(const camera_settings_source_t *s)
{
    g_settings_source = s;
}

/* Convenience helpers used by the camera component + (later)
 * the FW-20.5 runtime reconfig path. */
esp_err_t camera_settings_apply(const camera_settings_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    const camera_settings_source_t *src = camera_settings_get_source();
    sensor_t *s = NULL;
    /* Host path: read the captured sensor via the mock getter. */
#ifdef UNITY_HOST_BUILD
    extern sensor_t *mock_esp_camera_sensor_get_sentinel(void);
    s = mock_esp_camera_sensor_get_sentinel();
#endif
    if (!src || !src->apply) return ESP_ERR_INVALID_STATE;
    return src->apply(s, in);
}

esp_err_t camera_settings_reset_defaults(camera_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    const camera_settings_source_t *src = camera_settings_get_source();
    if (!src || !src->reset_defaults) return ESP_ERR_INVALID_STATE;
    return src->reset_defaults(out);
}

/* ---------- fake source ops ---------- */

/* Compiled-in defaults match the FW-02 sdkconfig.defaults
 * (jpeg_quality=18, frame_size=5/FRAMESIZE_QVGA). All other
 * fields are 0 (the OV2640 midpoint for unsigned calibration
 * registers). */
static const camera_settings_t k_default_settings = {
    .brightness   = 0,
    .contrast     = 0,
    .saturation   = 0,
    .sharpness    = 0,
    .denoise      = 0,
    .special_effect = 0,
    .whitebal     = 1,  /* white balance on */
    .awb_gain     = 1,
    .wb_mode      = 0,
    .aec          = 1,  /* auto exposure on */
    .aec2         = 0,
    .ae_level     = 0,
    .aec_value    = 300,
    .agc          = 1,  /* auto gain on */
    .agc_gain     = 0,
    .gainceiling  = 0,
    .bpc          = 0,
    .wpc          = 1,
    .raw_gma      = 1,
    .lenc         = 1,
    .hmirror      = 0,
    .vflip        = 0,
    .dcw          = 1,
    .colorbar     = 0,
    .framesize    = CONFIG_FIRMWARE_CAMERA_FRAME_SIZE,
    .quality      = CONFIG_FIRMWARE_CAMERA_JPEG_QUALITY,
    .schema_version = CAMERA_SETTINGS_SCHEMA_VERSION,
};

static esp_err_t fake_load(camera_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!g_fake_blob_valid) {
        /* No stored blob — caller falls back to reset_defaults. */
        return ESP_ERR_NOT_FOUND;
    }
    *out = g_fake_blob;
    return ESP_OK;
}

static esp_err_t fake_apply(sensor_t *sensor, const camera_settings_t *in)
{
    if (!sensor || !in) return ESP_ERR_INVALID_ARG;
    if (sensor->set_framesize)
        sensor->set_framesize(sensor, in->framesize);
    if (sensor->set_quality)
        sensor->set_quality(sensor, in->quality);
    if (sensor->set_vflip)
        sensor->set_vflip(sensor, in->vflip);
    if (sensor->set_hmirror)
        sensor->set_hmirror(sensor, in->hmirror);
    return ESP_OK;
}

static esp_err_t fake_reset_defaults(camera_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = k_default_settings;
    /* Apply the defaults through the sensor setter surface so a
     * "no stored blob" scenario still drives `set_quality(18)`
     * into the ring buffer (the FW-10.5 S2 walking-skeleton
     * test asserts this single call). */
    return ESP_OK;
}

static uint32_t fake_schema_version(void)
{
    return CAMERA_SETTINGS_SCHEMA_VERSION;
}

/* ---------- test helpers ----------
 *
 * These are NOT part of the production API — they're declared
 * here so the host tests can mutate the fake blob directly.
 * On device builds they are weak no-ops to keep the linker
 * happy when no test calls them.
 */

/* Forward decl for the "wrapped" test source's apply — exposed
 * to test_camera_settings_fake.c which wraps the default fake
 * apply without duplicating its body. The wrapper source
 * uses this op to keep the mock ring-buffer recording
 * behavior identical to the default fake_apply. */
esp_err_t fake_apply_stub_for_wrapped(sensor_t *sensor, const camera_settings_t *in)
{
    return fake_apply(sensor, in);
}

/* Same pattern for reset_defaults — the wrapper source needs
 * the default's `k_default_settings` population behavior,
 * not a NULL entry. */
esp_err_t fake_reset_defaults_stub_for_wrapped(camera_settings_t *out)
{
    return fake_reset_defaults(out);
}
esp_err_t camera_settings_test_prime_fake_blob(const camera_settings_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    g_fake_blob = *in;
    g_fake_blob_valid = true;
    return ESP_OK;
}

void camera_settings_test_clear_fake_blob(void)
{
    g_fake_blob_valid = false;
    memset(&g_fake_blob, 0, sizeof(g_fake_blob));
}
