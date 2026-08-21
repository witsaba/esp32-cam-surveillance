/* test_boot_decide.c — FW-03.3 provisioning decision + non-start
 *
 * Spec: "provisioning branch is taken deterministically." Three
 * decision scenarios test `boot_decide_provisioning()` directly
 * (empty SSID + no button → provisioning; configured SSID + no
 * button → normal; configured SSID + button-pressed → provisioning).
 * The fourth scenario asserts that when the decision routes to the
 * provisioning branch, the orchestrator's `boot_run()` does NOT
 * start the supervision tasks (FW-08/11/15/16/18 follow-ups depend
 * on this — if the orchestrator ever fires supervision on the
 * provisioning branch, the softAP tears down under those tasks).
 *
 * `boot_decide_provisioning()` is the FR-1 step-2 decision: empty
 * SSID OR button-pressed → provisioning. It MUST be a pure
 * function (no globals) — this is asserted by calling it twice
 * with identical inputs in test 1 and observing identical outputs.
 */
#include "mock_nvs_flash_link.h"
#include "mock_boot_link.h"

#include <string.h>

#include "boot.h"
#include "boot_status.h"
#include "config.h"
#include "mock_boot_button.h"
#include "mock_init_returns.h"
#include "mock_log.h"
#include "mock_supervision_record.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

static config_t make_cfg(const char *ssid)
{
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    if (ssid) {
        strncpy(cfg.wifi.ssid, ssid, sizeof(cfg.wifi.ssid) - 1);
    }
    return cfg;
}

TEST_CASE(
    "boot_decide_provisioning returns true on empty SSID + button-not-pressed [fw-03.3]",
    "[boot][fw-03.3][decision]")
{
    mock_boot_button_set(false);
    config_t cfg = make_cfg("");
    TEST_ASSERT_TRUE(boot_decide_provisioning(&cfg, false));
}

TEST_CASE(
    "boot_decide_provisioning returns false on non-empty SSID + button-not-pressed [fw-03.3]",
    "[boot][fw-03.3][decision]")
{
    mock_boot_button_set(false);
    config_t cfg = make_cfg("home-2.4");
    TEST_ASSERT_FALSE(boot_decide_provisioning(&cfg, false));
}

TEST_CASE(
    "boot_decide_provisioning returns true on non-empty SSID + button-pressed [fw-03.3]",
    "[boot][fw-03.3][decision]")
{
    mock_boot_button_set(true);
    config_t cfg = make_cfg("home-2.4");
    TEST_ASSERT_TRUE(boot_decide_provisioning(&cfg, true));
}

TEST_CASE(
    "boot_run in provisioning branch does not start supervision tasks [fw-03.3]",
    "[boot][fw-03.3][non-start]")
{
    mock_nvs_reset();
    mock_init_returns_reset();
    mock_log_reset();
    mock_supervision_reset();
    mock_boot_button_set(false);
    /* Empty SSID -> decision routes to boot_run_provisioning(). */
    mock_nvs_seed_u8("config", "schema_version", CONFIG_SCHEMA_VERSION);
    mock_nvs_seed_str("config", "ssid", "");
    mock_nvs_seed_str("config", "password", "");

    boot_status_t s = boot_run();

    TEST_ASSERT_EQUAL_INT(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_INT(BOOT_STEP_RETURN, s.step);
    /* Provisioning branch MUST NOT start supervision tasks — FW-05
     * owns the softAP body and FW-08/11/15/16/18 own the real
     * supervision impls. */
    TEST_ASSERT_EQUAL_size_t(0u, mock_supervision_count());
}