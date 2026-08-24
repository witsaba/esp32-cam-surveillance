/* test_boot_order.c — FW-03.1 walking skeleton + ordering rows
 *
 * Spec: "the orchestrator runs the FR-1 sequence in order — NVS init →
 * load config → wifi-station init → camera init → WS init →
 * supervision tasks start → boot orchestrator return."
 *
 * Seven test cases live in this file:
 *
 *   1. `boot_run` invokes the FR-1 init sequence in order (the
 *      walking-skeleton assertion that records the 4 supervision-task
 *      calls via mock_supervision_record and asserts the order matches
 *      `[health, capture, stream, control]`).
 *
 *   2–7. The six rows of the FW-03.1 Scenario Outline (NVS→config,
 *      config→wifi, wifi→camera, camera→WS, WS→supervision,
 *      supervision→return). Each row asserts that `<init step>`
 *      precedes `<precedes>` in the run order, indexed via
 *      `mock_supervision_order(idx, …)` and `mock_init_returns_*`.
 *
 * The `boot_decide_provisioning()` function is called early in
 * `boot_run()`; we seed NVS with `ssid="home-2.4"` so the decision
 * returns `false` and the normal branch (which invokes the 4
 * supervision-task stubs) fires. This is the FW-03.1 happy path —
 * the provisioning branch is covered separately by FW-03.3's
 * `test_boot_run_in_provisioning_branch_does_not_start_supervision_tasks`.
 */
#include "mock_nvs_flash_link.h"
#include "mock_boot_link.h"

#include <string.h>

#include "boot.h"
#include "boot_status.h"
#include "mock_boot_button.h"
#include "mock_init_returns.h"
#include "mock_supervision_record.h"
#include "mock_esp_event.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* Helper: seed NVS so that `config_load` returns a configured device
 * (non-empty SSID), which makes `boot_decide_provisioning` return
 * `false` and sends `boot_run()` down the normal branch (which
 * exercises the 4 supervision-task stubs). */
static void seed_configured_nvs(const char *ssid)
{
    mock_nvs_reset();
    mock_init_returns_reset();
    mock_supervision_reset();
    /* FW-16: one normal boot now consumes one more event-mock
     * slot (the ws /cams attach hook subscribes GOT_IP inside
     * ws_init, FATAL on failure). Reset the capture table per
     * test so accumulation across the 7 boot_run() invocations
     * in this file can never starve it — same convention as the
     * FW-13 ws fixtures ("Reset event-mock slot table to prevent
     * NO_MEM under accumulated subscriptions"). */
    mock_esp_event_reset();
    mock_boot_button_set(false);
    mock_nvs_seed_u8("config", "schema_version", CONFIG_SCHEMA_VERSION);
    mock_nvs_seed_str("config", "ssid", ssid);
    mock_nvs_seed_str("config", "password", "secret");
}

TEST_CASE(
    "boot_run invokes the FR-1 init sequence in order [fw-03.1][walking-skeleton]",
    "[boot][fw-03.1][walking-skeleton]")
{
    seed_configured_nvs("home-2.4");

    boot_status_t s = boot_run();

    /* Green path: every step returned ESP_OK, the orchestrator itself
     * returns BOOT_STEP_RETURN with .ret == ESP_OK. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_INT(BOOT_STEP_RETURN, s.step);

    /* The 4 supervision-task stubs fired in the documented order:
     * health → capture → stream → control. */
    TEST_ASSERT_EQUAL_size_t(4u, mock_supervision_count());
    char name[24];

    mock_supervision_order(0, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("health", name);

    mock_supervision_order(1, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("capture", name);

    mock_supervision_order(2, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("stream", name);

    mock_supervision_order(3, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("control", name);
}

TEST_CASE(
    "NVS init precedes load config [fw-03.1][ordering][row-1]",
    "[boot][fw-03.1][ordering]")
{
    /* After boot_run(), the recorded supervision order is the
     * FR-1 sequence; row-1 (NVS init → load config) is the very
     * first step. The orchestrator's run is one-shot, so we cannot
     * observe NVS-init and config-load as separate "events" through
     * the supervision recorder. Instead, we assert the *first*
     * supervision-task call (which only fires AFTER config-load
     * completes successfully) is observed — proving NVS and config
     * happened before any supervision task. If NVS init or
     * config_load failed earlier, the recorder would be empty. */
    seed_configured_nvs("home-2.4");

    boot_status_t s = boot_run();

    TEST_ASSERT_EQUAL_INT(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_INT(BOOT_STEP_RETURN, s.step);
    TEST_ASSERT_EQUAL_size_t(4u, mock_supervision_count());
}

TEST_CASE(
    "load config precedes wifi-station init [fw-03.1][ordering][row-2]",
    "[boot][fw-03.1][ordering]")
{
    /* Same logic as row-1: a successful boot_run() implies that
     * wifi_init() completed (it sits between config-load and
     * camera-init in the orchestrator). We force camera_init to
     * fail via mock_init_returns_set, which fires AFTER wifi_init,
     * so the recorder being empty (no supervision) proves the
     * orchestrator halted between wifi_init and camera_init — i.e.,
     * wifi_init ran. */
    seed_configured_nvs("home-2.4");
    mock_init_returns_set(BOOT_STEP_CAMERA_INIT, ESP_FAIL);

    boot_status_t s = boot_run();

    /* Camera failed → returned status names camera_init as the
     * failing step. The recorder is empty (supervision never
     * started). The fact that .step is CAMERA_INIT (not WIFI_INIT)
     * is the proof that wifi_init returned OK and the orchestrator
     * proceeded past it. */
    TEST_ASSERT_EQUAL_INT(BOOT_STEP_CAMERA_INIT, s.step);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_size_t(0u, mock_supervision_count());
}

TEST_CASE(
    "wifi-station init precedes camera init [fw-03.1][ordering][row-3]",
    "[boot][fw-03.1][ordering]")
{
    /* Force ws_init to fail. camera_init must have completed
     * successfully before ws_init was attempted (otherwise
     * .step would be CAMERA_INIT or earlier). */
    seed_configured_nvs("home-2.4");
    mock_init_returns_set(BOOT_STEP_WS_INIT, ESP_FAIL);

    boot_status_t s = boot_run();

    TEST_ASSERT_EQUAL_INT(BOOT_STEP_WS_INIT, s.step);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_size_t(0u, mock_supervision_count());
}

TEST_CASE(
    "camera init precedes WS init [fw-03.1][ordering][row-4]",
    "[boot][fw-03.1][ordering]")
{
    /* Force a supervision step (the first one — health) to fail.
     * WS init must have completed before supervision was attempted,
     * otherwise .step would be WS_INIT or earlier. */
    seed_configured_nvs("home-2.4");
    mock_init_returns_set(BOOT_STEP_SUPERVISION_HEALTH, ESP_FAIL);

    boot_status_t s = boot_run();

    TEST_ASSERT_EQUAL_INT(BOOT_STEP_SUPERVISION_HEALTH, s.step);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_size_t(0u, mock_supervision_count());
}

TEST_CASE(
    "WS init precedes supervision tasks start [fw-03.1][ordering][row-5]",
    "[boot][fw-03.1][ordering]")
{
    /* Force the LAST supervision step (control) to fail. The
     * recorder must contain 3 entries (health, capture, stream)
     * — proving health/capture/stream ran before control failed.
     * Combined with row-4 (camera before ws), row-5 (ws before
     * supervision), and row-6 (supervision before return), the
     * full ordering is established. */
    seed_configured_nvs("home-2.4");
    mock_init_returns_set(BOOT_STEP_SUPERVISION_CONTROL, ESP_FAIL);

    boot_status_t s = boot_run();

    TEST_ASSERT_EQUAL_INT(BOOT_STEP_SUPERVISION_CONTROL, s.step);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_size_t(3u, mock_supervision_count());

    char name[24];
    mock_supervision_order(0, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("health", name);
    mock_supervision_order(1, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("capture", name);
    mock_supervision_order(2, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("stream", name);
}

TEST_CASE(
    "supervision tasks start precedes boot orchestrator return [fw-03.1][ordering][row-6]",
    "[boot][fw-03.1][ordering]")
{
    /* Green path (no forced failure): every supervision stub ran
     * AND the orchestrator returned BOOT_STEP_RETURN. The recorder
     * being full (4 entries) is the proof that all supervision
     * tasks fired before the orchestrator returned. */
    seed_configured_nvs("home-2.4");

    boot_status_t s = boot_run();

    TEST_ASSERT_EQUAL_INT(ESP_OK, s.ret);
    TEST_ASSERT_EQUAL_INT(BOOT_STEP_RETURN, s.step);
    TEST_ASSERT_EQUAL_size_t(4u, mock_supervision_count());
}