/* test_config_guard.c — FW-02.3 guard bite-proof
 *
 * Spec: "the version check MUST be observable to tests." The schema
 * guard is implemented behind `#ifndef CONFIG_TEST_STUB_VERSION_CHECK`
 * in `config.c`'s `config_schema_is_stale()`. The bite-proof test
 * asserts that stubbing the check (via `-DCONFIG_TEST_STUB_VERSION_CHECK`
 * on the compile command) makes the stale-schema scenario FAIL — which
 * proves the check is load-bearing.
 *
 * Two test cases in this file:
 *
 *   1. `test_stale_schema_rejected_when_check_stubbed`
 *      Seeds `schema_version = 0` (stale) + a non-default ssid,
 *      calls `config_load`, and asserts the recovery contract:
 *        - return code == CONFIG_OK
 *        - dirty == true (proves the boot orchestrator will save)
 *        - ssid == '\0' (proves stale data was wiped)
 *
 *      When the host runner compiles THIS FILE WITH
 *      `-DCONFIG_TEST_STUB_VERSION_CHECK`, the stub disables the
 *      version check, so `config_load` reads the stale strings as
 *      if they were valid. The assertions fail — that is the
 *      bite-proof. The test name contains the literal
 *      `schema_version` substring (per the milestones doc bite-proof
 *      requirement), so the failure message also contains it.
 *
 *   2. `test_matching_schema_passes`
 *      Seeds `schema_version = CONFIG_SCHEMA_VERSION` and asserts
 *      `dirty == false`. Always passes — both with and without the
 *      stub (because under the stub every schema is treated as
 *      fresh).
 *
 * Both tests live in the SAME source file so the host runner can
 * compile it twice (with and without the stub) and report the
 * bite-proof verdict.
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
    "stale schema_version is rejected when check runs "
    "(guard bite-proof: fails if CONFIG_TEST_STUB_VERSION_CHECK is set)",
    "[config][fw-02.3][guard][bite-proof]")
{
    mock_nvs_reset();

    /* Stale stored schema_version. Under the real check, this
     * triggers the defaults-and-dirty recovery. Under the stub,
     * the check is bypassed and the stale strings are returned as
     * if they were valid — the assertions below fail. */
    mock_nvs_seed_u8("config", "schema_version", 0);
    mock_nvs_seed_str("config", "ssid", "stale-network");
    mock_nvs_seed_str("config", "password", "stale-secret");

    config_t cfg;
    bool dirty = false;
    config_status_t st = config_load(&cfg, &dirty);

    /* Without the stub: stale check fires → defaults + dirty. */
    TEST_ASSERT_EQUAL_INT(CONFIG_OK, st);
    TEST_ASSERT_TRUE(dirty);
    TEST_ASSERT_EQUAL_INT('\0', cfg.wifi.ssid[0]);
    TEST_ASSERT_EQUAL_INT('\0', cfg.wifi.password[0]);
}

TEST_CASE(
    "matching schema_version passes without dirty flag",
    "[config][fw-02.3][guard]")
{
    mock_nvs_reset();

    mock_nvs_seed_u8("config", "schema_version", CONFIG_SCHEMA_VERSION);

    config_t cfg;
    bool dirty = true;  /* must be cleared by load */
    config_status_t st = config_load(&cfg, &dirty);

    TEST_ASSERT_EQUAL_INT(CONFIG_OK, st);
    TEST_ASSERT_FALSE(dirty);
}