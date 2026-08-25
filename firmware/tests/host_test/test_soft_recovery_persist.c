/* test_soft_recovery_persist.c — FW-16.2 forensic reason persistence
 * + next-boot surfacing (R-FW16-1.2; design AD4/AD5).
 *
 * Proven on the multi-namespace in-memory NVS mock:
 *
 *   - P1 write-before-restart: the persisted reason is readable
 *     from namespace "recovery" while ZERO esp_restart() calls
 *     have been observed (ordering half of the sequence rule);
 *   - P2 factory-reset survival: wholesale erase of namespace
 *     "config" (config_factory_reset) leaves the "recovery"
 *     namespace intact BY CONSTRUCTION;
 *   - P3 next-boot equality: after a stored reason exists,
 *     health_log_last_recovery_reason() surfaces exactly that
 *     value (asserted through the host log capture);
 *   - P4 silent miss: with nothing stored, the reader logs
 *     NOTHING (NOT_FOUND → silent per AD4) and never warns.
 */

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include <nvs.h>

#include "health.h"
#include "config.h"

#include "mock_nvs_flash.h"
#include "mock_esp_system.h"
#include "mock_log.h"

#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#define RECOVERY_NS   "recovery"
#define RECOVERY_KEY  "last_recovery_reason"
#define REASON_STR    "soft_recovery_threshold"

/* Read back the stored reason from the mock store. Returns true on
 * a clean hit with buf filled NUL-terminated. */
static bool read_back_reason(char *buf, size_t cap)
{
    nvs_handle_t h;
    if (mock_nvs_open(RECOVERY_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    esp_err_t err = mock_nvs_get_str(h, RECOVERY_KEY, buf, &len);
    mock_nvs_close(h);
    return err == ESP_OK;
}

/* ---------- P1 — reason readable BEFORE any restart observed ---------- */
TEST_CASE(
    "test_soft_recovery_persist_reason_readable_before_any_restart [fw-16.2][persist][scenario-P1]",
    "[health][fw-16.2][persist]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_log_reset();

    /* Trigger-path persist step (runs before any esp_restart). */
    health_persist_last_recovery_reason();

    char buf[64] = {0};
    TEST_ASSERT_TRUE_MESSAGE(
        read_back_reason(buf, sizeof(buf)),
        "persist failed: last_recovery_reason not readable in ns \"recovery\"");
    TEST_ASSERT_EQUAL_STRING(REASON_STR, buf);

    /* Ordering: the reason MUST be readable while no restart has
     * been observed yet (persist strictly precedes esp_restart). */
    TEST_ASSERT_EQUAL_INT(0, mock_esp_restart_call_count());
}

/* ---------- P2 — factory reset of ns "config" leaves reason intact ---------- */
TEST_CASE(
    "test_soft_recovery_persist_survives_config_namespace_erase [fw-16.2][persist][scenario-P2]",
    "[health][fw-16.2][persist]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_log_reset();

    health_persist_last_recovery_reason();

    /* Wholesale erase of the CONFIG namespace (the FW-07 runtime
     * factory-reset path). This must NOT touch ns "recovery". */
    TEST_ASSERT_EQUAL(CONFIG_OK, config_factory_reset());

    char buf[64] = {0};
    TEST_ASSERT_TRUE_MESSAGE(
        read_back_reason(buf, sizeof(buf)),
        "forensic reason lost across config_factory_reset");
    TEST_ASSERT_EQUAL_STRING(REASON_STR, buf);

    /* Sanity: prove the erase REALLY happened (config schema key is
     * gone) so the survival assertion above is load-bearing. */
    uint8_t v = 0;
    TEST_ASSERT_NOT_EQUAL(
        ESP_OK, mock_nvs_read_u8("config", "schema_version", &v));
}

/* ---------- P3 — next boot surfaces exactly the stored value ---------- */
TEST_CASE(
    "test_soft_recovery_next_boot_logs_stored_reason_verbatim [fw-16.2][surface][scenario-P3]",
    "[health][fw-16.2][surface]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_log_reset();

    health_persist_last_recovery_reason();

    /* Next boot, right after config_load succeeds. */
    health_log_last_recovery_reason();

    /* Surfaced/logged value EQUALS the stored value. */
    TEST_ASSERT_TRUE_MESSAGE(
        strstr(mock_log_last_info, REASON_STR) != NULL,
        "boot surfacing did not log the stored reason verbatim");
    TEST_ASSERT_EQUAL_size_t(1, mock_log_info_count);
}

/* ---------- P4 — no stored reason: reader is silent ---------- */
TEST_CASE(
    "test_soft_recovery_surface_silent_when_no_reason_stored [fw-16.2][surface][scenario-P4]",
    "[health][fw-16.2][surface]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_log_reset();

    health_log_last_recovery_reason();

    /* NOT_FOUND → silent: no info line, no warn, no error, and
     * boot flow unaffected (reader is best-effort void). */
    TEST_ASSERT_EQUAL_size_t(0, mock_log_info_count);
    TEST_ASSERT_EQUAL_size_t(0, mock_log_warn_count);
    TEST_ASSERT_EQUAL_size_t(0, mock_log_error_count);
}
