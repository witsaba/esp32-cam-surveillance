/* test_config_schema_persists.c — FW-02.2 scenario b
 *
 * Spec: "next save after dirty load persists the new schema."
 * Given an in-memory config that became dirty after a stale-schema
 * load, `config_save` writes the compiled-in schema_version (1)
 * to NVS, AND the next `config_load` returns CONFIG_OK with
 * `dirty == false` (because the stored schema now matches the
 * compiled-in value).
 *
 * This is the second half of the FW-02.2 recovery contract: the
 * boot orchestrator (FW-03) sees dirty == true, calls
 * `config_save(&cfg)`, and the next boot reads a clean schema.
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
    "save after stale-schema load persists the compiled-in schema_version",
    "[config][fw-02.2][persistence]")
{
    mock_nvs_reset();

    /* Simulate a previous boot that left a stale schema in NVS. */
    mock_nvs_seed_u8("config", "schema_version", 0);

    /* Boot path: load (dirty=true), then save. */
    config_t cfg;
    bool dirty = false;
    config_status_t st = config_load(&cfg, &dirty);
    TEST_ASSERT_EQUAL_INT(CONFIG_OK, st);
    TEST_ASSERT_TRUE(dirty);

    /* Mutate a field so the save actually does something. */
    const char *new_ssid = "lab-network-a";
    strncpy(cfg.wifi.ssid, new_ssid, sizeof(cfg.wifi.ssid) - 1);

    TEST_ASSERT_EQUAL_INT(CONFIG_OK, config_save(&cfg));

    /* Verify the stored schema_version now matches compiled-in. */
    uint8_t stored = 0;
    TEST_ASSERT_EQUAL_INT(CONFIG_OK,
                          mock_nvs_read_u8("config", "schema_version", &stored));
    TEST_ASSERT_EQUAL_UINT8(CONFIG_SCHEMA_VERSION, stored);

    /* Next boot: load returns clean schema, dirty == false. */
    config_t cfg2;
    bool dirty2 = true;
    TEST_ASSERT_EQUAL_INT(CONFIG_OK, config_load(&cfg2, &dirty2));
    TEST_ASSERT_FALSE(dirty2);
    TEST_ASSERT_EQUAL_STRING(new_ssid, cfg2.wifi.ssid);
}