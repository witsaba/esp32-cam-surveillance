/* test_boot_stability_guard.c — FW-03.4 determinism guard
 *
 * Spec: "the provisioning decision is stable after the first
 * decision." The bite-proof asserts that under
 * -DBOOT_TEST_STUB_FLIP_DECISION, a stub-and-flip variant of
 * boot_decide_provisioning() returns alternating values across
 * calls — the green-path determinism invariant
 * `boot_decide_provisioning(cfg, b) == boot_decide_provisioning(cfg, b)`
 * fails, and the failure message names the violated invariant.
 *
 * The test calls boot_decide_provisioning() twice with identical
 * inputs and asserts the two returns are equal. Without the stub
 * flip (production build), both calls return the same value — the
 * test passes. With the stub flip (Pass 3 build), the function
 * alternates — the test fails with a message containing
 * "determinism".
 */
#include <string.h>

#include "boot.h"
#include "boot_status.h"
#include "config.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

TEST_CASE(
    "boot_decide_provisioning is deterministic across calls [fw-03.4][guard][bite-proof]",
    "[boot][fw-03.4][guard][bite-proof]")
{
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    /* Empty SSID — non-trivial decision path. */

    bool call1 = boot_decide_provisioning(&cfg, false);
    bool call2 = boot_decide_provisioning(&cfg, false);

    /* The invariant under test: same inputs MUST yield the same
     * output. If boot_decide_provisioning flips (e.g. via the
     * -DBOOT_TEST_STUB_FLIP_DECISION stub), the assertion below
     * fails and the failure message names the invariant so the
     * stub-build runner can assert "determinism" appears. */
    TEST_ASSERT_MESSAGE(call1 == call2,
        "boot_decide_provisioning determinism invariant violated");
}

/* The green-path test verifies the determinism invariant under the
 * real implementation (no stub flip). It is excluded from the stub
 * build because the flip ignores inputs entirely — any two-call
 * sequence would deterministically fail. The bite-proof is what
 * the stub build verifies; this green test only runs in the
 * production build (Pass 1). */
#ifndef BOOT_TEST_STUB_FLIP_DECISION
TEST_CASE(
    "boot_decide_provisioning returns same value twice when stub absent [fw-03.4][green]",
    "[boot][fw-03.4][green]")
{
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    strncpy(cfg.wifi.ssid, "home-2.4", sizeof(cfg.wifi.ssid) - 1);

    bool call1 = boot_decide_provisioning(&cfg, false);
    bool call2 = boot_decide_provisioning(&cfg, false);

    TEST_ASSERT_FALSE(call1);
    TEST_ASSERT_FALSE(call2);
    TEST_ASSERT_EQUAL_HEX32(call1, call2);
}
#endif /* BOOT_TEST_STUB_FLIP_DECISION */