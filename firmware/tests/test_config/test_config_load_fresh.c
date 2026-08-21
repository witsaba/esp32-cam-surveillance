/* test_config_load_fresh.c — FW-02.1 scenario a
 *
 * Spec: "fresh partition returns defaults and CONFIG_OK". Given an
 * erased NVS namespace, `config_load` returns CONFIG_OK, writes the
 * defaults into the out-parameter, and marks the in-memory config
 * dirty so the next `config_save` persists the compiled-in schema.
 *
 * Includes `mock_nvs_flash_link.h` BEFORE `config.h` so that the
 * `#define nvs_open mock_nvs_open` macros redirect the production
 * code's NVS calls to the in-memory mock. The mock's `mock_nvs_reset()`
 * starts every test from an empty store, which is the fresh-partition
 * precondition.
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

TEST_CASE("config_load on a fresh partition returns defaults and marks dirty",
          "[config][fw-02.1]")
{
    mock_nvs_reset();

    config_t cfg;
    bool dirty = false;
    config_status_t st = config_load(&cfg, &dirty);

    /* Fresh partition is a recoverable condition, not an error. */
    TEST_ASSERT_EQUAL_INT(CONFIG_OK, st);
    /* Dirty flag is set so the boot orchestrator (FW-03) knows the
     * compiled-in schema needs to be persisted on the next save. */
    TEST_ASSERT_TRUE(dirty);
    /* Every string field defaults to empty. */
    TEST_ASSERT_EQUAL_INT('\0', cfg.wifi.ssid[0]);
    TEST_ASSERT_EQUAL_INT('\0', cfg.wifi.password[0]);
    TEST_ASSERT_EQUAL_INT('\0', cfg.identity.name[0]);
    TEST_ASSERT_EQUAL_INT('\0', cfg.identity.description[0]);
    /* The compiled-in schema_version is reflected in the struct. */
    TEST_ASSERT_EQUAL_UINT8(CONFIG_SCHEMA_VERSION, cfg.schema_version);
}