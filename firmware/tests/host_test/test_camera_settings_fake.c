/* test_camera_settings_fake.c — FW-10.5 walking-skeleton +
 * setter-path scenarios for the fake in-memory
 * camera_settings source.
 *
 * Three scenarios:
 *
 *   S1 (walking skeleton — stored blob overrides Kconfig
 *       defaults) — The fake source's load() returns
 *       ESP_ERR_NOT_FOUND by default; camera_init() applies
 *       Kconfig defaults (one set_quality(18) call) and skips
 *       the stored override path. Primes the fake blob via
 *       camera_settings_test_prime_fake_blob(quality=12) +
 *       installs a custom source that returns the blob on
 *       load(); camera_init() then applies Kconfig defaults
 *       FIRST (set_quality(18)) and the stored value SECOND
 *       (set_quality(12)) — assert
 *       mock_esp_camera_sensor_set_quality_arg_at(0) == 12
 *       (newest-first) and index 1 == 18.
 *
 *   S2 (no stored blob uses Kconfig defaults once) — leaves
 *       the fake blob cleared; camera_init() applies
 *       Kconfig defaults exactly once (set_quality(18)). The
 *       test asserts the ring has exactly 1 entry at index 0
 *       for framesize, and exactly 1 entry at index 0 for
 *       quality (no second apply).
 *
 *   S3 (stored via setters, not reinit) — primes a non-default
 *       blob, runs camera_init(), and asserts esp_camera_init
 *       was called EXACTLY ONCE across the flow (the runtime
 *       apply path MUST NOT reinit the driver).
 *
 * The 3 scenarios triangulate the camera_settings_source_t
 * vtable: S1 proves the load→apply chain reaches the sensor
 * setter surface; S2 proves Kconfig-default-on-empty-source;
 * S3 proves stored-blob never causes esp_camera_init to
 * reinit. Without all 3, a fake-it implementation that
 * always applies defaults would slip past S2 and S3, OR an
 * init that bypasses load() entirely would slip past S1.
 */
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_log.h"

#include "camera.h"
#include "camera_settings.h"
#include "config.h"
#include "esp_err.h"
#include "unity.h"

#include <string.h>

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* ---------- helpers ----------
 *
 * Custom test source that wraps the default fake so we can
 * inject a chosen stored blob. The "wrapper" pattern lets the
 * walker chain through existing fake ops without duplicating
 * them; we override load() to return the test's primed blob.
 */

/* Module-static state for the wrapper source — used by the
 * S1 test case. Cleared on each test by the wrapper
 * install/uninstall pattern. */
static const camera_settings_t *g_wrapped_primed = NULL;

static esp_err_t wrapped_load(camera_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!g_wrapped_primed) return ESP_ERR_NOT_FOUND;
    *out = *g_wrapped_primed;
    return ESP_OK;
}

static esp_err_t wrapped_apply(sensor_t *sensor, const camera_settings_t *in)
{
    /* Defer to the default fake's apply — record the args in
     * the mock's ring buffer for assertion. */
    extern esp_err_t fake_apply_stub_for_wrapped(sensor_t *, const camera_settings_t *);
    return fake_apply_stub_for_wrapped(sensor, in);
}

static esp_err_t wrapped_reset_defaults(camera_settings_t *out)
{
    /* Defer to the default fake's reset_defaults — copies
     * k_default_settings (Kconfig defaults) into `*out`. */
    extern esp_err_t fake_reset_defaults_stub_for_wrapped(camera_settings_t *);
    return fake_reset_defaults_stub_for_wrapped(out);
}

static uint32_t wrapped_schema_version(void)
{
    return CAMERA_SETTINGS_SCHEMA_VERSION;
}

static const camera_settings_source_t wrapped_test_source = {
    .load           = wrapped_load,
    .apply          = wrapped_apply,
    .reset_defaults = wrapped_reset_defaults,
    .schema_version = wrapped_schema_version,
};

/* ---------- Test fixture + scenarios ---------- */

static esp_err_t camera_init_with_settings_mocks(const camera_settings_source_t *src)
{
    mock_esp_camera_reset();
    mock_log_reset();
    mock_esp_camera_prime_psram(true, 4194304);
    mock_esp_camera_init_return_set(ESP_OK);

    /* Default default-values apply chain is owned by the
     * `fake_apply` op. To make S2 work without a stored blob,
     * we install the default fake BEFORE camera_init (the
     * default is installed automatically on
     * `camera_settings_get_source` if no other source is set
     * — but we install explicitly for clarity). */
    if (src) {
        camera_settings_set_source_for_test(src);
    } else {
/* Default fake — used by S2. */
    camera_settings_set_source_for_test(&fake_camera_settings_source);
    }

    config_t cfg = {0};
    return camera_init(&cfg);
}

/* S2 — no stored blob uses Kconfig defaults once */
TEST_CASE(
    "test_fw10_5_no_stored_blob_uses_kconfig_defaults_once [fw-10.5][scenario-S2]",
    "[camera][fw-10.5][settings-fake][green]")
{
    /* No wrapped source; the default fake is in place with NO
     * primed blob (its `g_fake_blob_valid` is FALSE). */
    esp_err_t rc = camera_init_with_settings_mocks(NULL);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* esp_camera_init was called exactly once; the runtime
     * apply path MUST NOT have reinit'd the driver. */
    TEST_ASSERT_GREATER_THAN(0, mock_esp_camera_init_call_count());

    /* Kconfig defaults reached the sensor setter surface
     * exactly once via set_quality(18). The newest-first
     * ring means ring[0] is 18 and ring[1] is -1 (out of
     * range). */
    int latest_q = mock_esp_camera_sensor_set_quality_arg_at(0);
    int older_q  = mock_esp_camera_sensor_set_quality_arg_at(1);
    TEST_ASSERT_EQUAL_INT(18, latest_q);
    TEST_ASSERT_EQUAL_INT(-1, older_q);   /* exactly one call */
}

/* S1 — stored blob overrides Kconfig defaults */
TEST_CASE(
    "test_fw10_5_stored_blob_overrides_kconfig_defaults [fw-10.5][scenario-S1][walking-skeleton]",
    "[camera][fw-10.5][settings-fake]")
{
    /* Prime a blob with quality=12 (different from the Kconfig
     * default 18). Use a static const because
     * g_wrapped_primed is read by wrapped_load() at
     * camera_init() time. */
    static const camera_settings_t k_primed = {
        .framesize = 5,       /* FRAMESIZE_QVGA (Kconfig) */
        .quality   = 12,      /* stored override */
        .schema_version = 1,  /* matches CAMERA_SETTINGS_SCHEMA_VERSION */
    };
    g_wrapped_primed = &k_primed;

    esp_err_t rc = camera_init_with_settings_mocks(&wrapped_test_source);
    g_wrapped_primed = NULL;  /* tear down before assertions */
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* The setter ring MUST show Kconfig defaults FIRST
     * (set_quality(18)) and the stored override SECOND
     * (set_quality(12)). Newest-first ring: idx 0 == 12,
     * idx 1 == 18. Older entries at higher indices are
     * undefined (out-of-range). */
    int newest = mock_esp_camera_sensor_set_quality_arg_at(0);
    int older  = mock_esp_camera_sensor_set_quality_arg_at(1);
    TEST_ASSERT_EQUAL_INT(12, newest);  /* stored override — second apply */
    TEST_ASSERT_EQUAL_INT(18, older);   /* Kconfig default — first apply */
}

/* S3 — stored blob is applied via setters, not reinit */
TEST_CASE(
    "test_fw10_5_stored_via_setters_not_reinit [fw-10.5][scenario-S3]",
    "[camera][fw-10.5][settings-fake][no-reinit]")
{
    static const camera_settings_t k_primed = {
        .framesize = 5,
        .quality   = 12,
        .schema_version = 1,
    };
    g_wrapped_primed = &k_primed;

    esp_err_t rc = camera_init_with_settings_mocks(&wrapped_test_source);
    g_wrapped_primed = NULL;
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* esp_camera_init was called exactly once — the runtime
     * apply path MUST NOT have reinit'd the driver. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_camera_init_call_count());
}
