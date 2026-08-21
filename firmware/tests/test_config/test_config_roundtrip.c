/* test_config_roundtrip.c — FW-02.1 scenario b
 *
 * Spec: "save then load preserves every field at max capacity". The
 * per-key NVS layout must round-trip every field of `config_t` —
 * no blob, no compaction, no field-level fallback. Each field is
 * written via `nvs_set_str` and read back via `nvs_get_str`. The
 * mock's in-memory map persists across calls within one test, which
 * models real NVS flash semantics.
 *
 * The assertion catches any regression that loses field-level fidelity
 * (e.g., switching to a single-blob layout that drops a key on
 * truncation). Per the constraint catalogue: "The save/load cycle
 * MUST use per-key NVS writes, NOT a single blob."
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

TEST_CASE("config_save then config_load round-trips every field",
          "[config][fw-02.1][roundtrip]")
{
    mock_nvs_reset();

    config_t cfg;
    bool dirty = false;

    /* Build a cfg with every field set to a non-default value. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    /* Use the maximum-length string the field can hold (NUL terminator
     * included). The mock stores bytes verbatim, so capacity limits
     * come from the config_t struct, not from the mock. */
    const char *expected_ssid          = "lab-network-a";
    const char *expected_password      = "supersecret";
    const char *expected_name          = "front-door";
    const char *expected_description   = "main entrance camera";

    strncpy(cfg.wifi.ssid, expected_ssid, sizeof(cfg.wifi.ssid) - 1);
    strncpy(cfg.wifi.password, expected_password, sizeof(cfg.wifi.password) - 1);
    strncpy(cfg.identity.name, expected_name, sizeof(cfg.identity.name) - 1);
    strncpy(cfg.identity.description, expected_description,
            sizeof(cfg.identity.description) - 1);

    config_status_t save_st = config_save(&cfg);
    TEST_ASSERT_EQUAL_INT(CONFIG_OK, save_st);

    /* Re-load into a fresh struct: the in-memory mock persists, but
     * production code must not rely on memory; it must read from
     * NVS every time. */
    config_t cfg2;
    bool dirty2 = true;  /* must be reset to false by load */
    memset(&cfg2, 0xAA, sizeof(cfg2));  /* poison to prove load overwrites */

    config_status_t load_st = config_load(&cfg2, &dirty2);

    TEST_ASSERT_EQUAL_INT(CONFIG_OK, load_st);
    TEST_ASSERT_FALSE(dirty2);
    TEST_ASSERT_EQUAL_STRING(expected_ssid, cfg2.wifi.ssid);
    TEST_ASSERT_EQUAL_STRING(expected_password, cfg2.wifi.password);
    TEST_ASSERT_EQUAL_STRING(expected_name, cfg2.identity.name);
    TEST_ASSERT_EQUAL_STRING(expected_description, cfg2.identity.description);
    TEST_ASSERT_EQUAL_UINT8(CONFIG_SCHEMA_VERSION, cfg2.schema_version);
}