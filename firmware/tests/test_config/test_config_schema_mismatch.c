/* test_config_schema_mismatch.c — FW-02.2 scenario a
 *
 * Spec: "stale stored schema falls back to defaults and marks dirty."
 * Given a stored `schema_version` older than the compiled-in
 * CONFIG_SCHEMA_VERSION, `config_load` returns CONFIG_OK, fills the
 * out-parameter with defaults, and sets `*out_dirty = true`. The
 * stale-schema scenario covers BOTH `stored < compiled` AND
 * `stored > compiled` (the latter catches future-version entries
 * that may have an unparseable layout).
 *
 * Per design #3570 "stale := stored != compiled-in" — defensive
 * default to refuse unknown layouts.
 */
#include "mock_nvs_flash_link.h"

#include <string.h>

#include "config.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

TEST_CASE(
    "stale schema_version returns defaults and sets dirty",
    "[config][fw-02.2]")
{
    mock_nvs_reset();

    /* Seed the mock with a stale schema version. Older than
     * CONFIG_SCHEMA_VERSION (which is 1). */
    mock_nvs_seed_u8("config", "schema_version", 0);
    /* Seed a bogus ssid so we can assert the load wipes it. */
    mock_nvs_seed_str("config", "ssid", "stale-network");
    mock_nvs_seed_str("config", "password", "stale-secret");

    config_t cfg;
    bool dirty = false;
    config_status_t st = config_load(&cfg, &dirty);

    TEST_ASSERT_EQUAL_INT(CONFIG_OK, st);
    TEST_ASSERT_TRUE(dirty);
    /* Defaults wipe any stale entries. */
    TEST_ASSERT_EQUAL_INT('\0', cfg.wifi.ssid[0]);
    TEST_ASSERT_EQUAL_INT('\0', cfg.wifi.password[0]);
    TEST_ASSERT_EQUAL_INT('\0', cfg.identity.name[0]);
    TEST_ASSERT_EQUAL_INT('\0', cfg.identity.description[0]);
    /* The compiled-in schema_version is still mirrored in the struct. */
    TEST_ASSERT_EQUAL_UINT8(CONFIG_SCHEMA_VERSION, cfg.schema_version);
}

TEST_CASE(
    "future-version stored schema also falls back to defaults (defensive)",
    "[config][fw-02.2]")
{
    /* A future-version entry may have a layout we cannot interpret.
     * Treat it as stale and recover rather than reading garbage. */
    mock_nvs_reset();

    mock_nvs_seed_u8("config", "schema_version", 99u);
    mock_nvs_seed_str("config", "ssid", "future-network");

    config_t cfg;
    bool dirty = false;
    config_status_t st = config_load(&cfg, &dirty);

    TEST_ASSERT_EQUAL_INT(CONFIG_OK, st);
    TEST_ASSERT_TRUE(dirty);
    TEST_ASSERT_EQUAL_INT('\0', cfg.wifi.ssid[0]);
}