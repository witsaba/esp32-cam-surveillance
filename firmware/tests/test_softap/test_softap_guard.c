/* test_softap_guard.c — FW-05.4 validation + bite-proof.
 *
 * The /provision handler's strict validation (non-JSON body, JSON
 * missing the two required wifi credentials) lives in
 * provision_post_handler_impl() in softap_handlers.c. These 6 host
 * Unity tests assert:
 *
 *   1. non_json_body              — body is not JSON → 400 err="json"
 *   2. missing_wifi_ssid          — JSON missing wifi_ssid → 400 err="wifi_ssid"
 *   3. missing_wifi_password      — JSON missing wifi_password → 400 err="password"
 *   4. accepts_missing_name       — JSON missing name → 200 (preserves NVS identity.name)
 *   5. accepts_missing_description— JSON missing desc  → 200 (preserves NVS identity.desc)
 *   6. well_formed_body           — JSON has all 4 keys → 200, save, reboot
 *
 * Each rejection test ALSO asserts `mock_nvs_write_count() == 0` and
 * `mock_esp_restart_call_count() == 0` — proves config_save() was
 * NEVER called and esp_restart() was NEVER called on the rejection
 * path (PRD § FR-1a + req-softap-004 invariant).
 *
 * The accept-missing-name/description tests use the seed-and-read-back
 * pattern (nvs_open + nvs_get_str) to assert that the in-memory cfg
 * loaded by boot_run() preserved the original name/description
 * through the partial-update merge path. FW-05.3 S2.
 *
 * Bite-proof: Pass 4 of run_host_tests.py compiles this file (and
 * softap_handlers.c) with -DSOFTAP_TEST_STUB_ACCEPT_ALL_BODIES=1,
 * which short-circuits the validation block in
 * provision_post_handler_impl(). Under the stub:
 *   - The 3 rejection tests (non-JSON, missing-wifi_ssid,
 *     missing-wifi_password) FAIL because the handler no longer
 *     returns 400 for malformed/missing-wifi-credentials bodies.
 *   - The 2 accept-missing-name/description tests still PASS because
 *     the merge path runs with absent keys → preserves from cfg
 *     → matches the test's seed.
 *   - The well-formed test continues to PASS.
 * So under stub: 3 fail + 3 pass.
 *
 * The `_MESSAGE` form of TEST_ASSERT_* is used for rejection tests so
 * the failure message printed by Unity contains "validation" —
 * required by the bite-proof runner assertion.
 */
#include "mock_nvs_flash_link.h"
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_http_server_link.h"
#include "mock_esp_system_link.h"

#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "boot_status.h"
#include "cJSON.h"
#include "config.h"
#include "softap.h"
#include "mock_esp_system.h"
#include "mock_http_server.h"
#include "mock_log.h"
#include "mock_nvs_flash.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* Bring up softAP once per test (registers the /provision handler)
 * and return a fresh primed req with the JSON body in
 * primed_recv_buffer. The caller invokes the handler.
 *
 * The optional `cfg` lets a test seed the in-memory identity fields
 * before bring-up (simulates boot_run() having loaded prior NVS
 * values). Pass NULL to start with a zeroed cfg.
 *
 * Each test begins with mock_nvs_reset() so the nvs_write_count
 * assertion in the rejection tests is reliable. */
static mock_httpd_req_t *drive_provision_guard(const char *body,
                                                 config_t *seed_cfg)
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_httpd_reset();
    mock_log_reset();

    const uint8_t mac[6] = {0xC8, 0xF0, 0x9E, 0x9D, 0x50, 0x08};
    mock_esp_read_mac_set_bytes(mac);
    mock_esp_chip_info_set(1, 3);
    mock_esp_get_idf_version_set("v5.5.3");

    config_t cfg;
    if (seed_cfg) {
        cfg = *seed_cfg;
    } else {
        memset(&cfg, 0, sizeof(cfg));
    }
    cfg.schema_version = CONFIG_SCHEMA_VERSION;

    boot_status_t s = softap_run_provisioning(&cfg);
    (void)s;

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);
    if (body && *body) {
        mock_httpd_req_set_primed_recv_buffer(req, body, strlen(body));
    } else {
        /* Empty body — set content_len to 0 so the handler's
         * read_post_body allocates a minimum buffer. */
        req->content_len = 0;
    }
    return req;
}

/* Common rejection-path assertions. All rejection tests call this
 * AFTER the handler returned. The MESSAGE form is required so the
 * bite-proof runner can grep for "validation" in the Unity output
 * when the stub build makes these assertions fail. */
static void assert_rejection_with_validation_msg(
    mock_httpd_req_t *req,
    const char *expected_err_key)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(
        req->captured_response_buffer,
        "validation: rejection path must send a response body");
    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, "validation: response must be JSON");
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "err");
    TEST_ASSERT_NOT_NULL_MESSAGE(err,
        "validation: response must include an err field naming the offending key");
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsString(err),
        "validation: err must be a string");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected_err_key, err->valuestring,
        "validation: err must name the offending key");
    cJSON_Delete(root);

    /* No NVS write on rejection path. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_nvs_write_count(),
        "validation: rejection path must NOT write to NVS");
    /* No esp_restart on rejection path. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mock_esp_restart_call_count(),
        "validation: rejection path must NOT call esp_restart");
}

/* Helper: assert response was 200 + {"ok":true}. */
static void assert_success_200_ok(mock_httpd_req_t *req)
{
    TEST_ASSERT_NOT_NULL(req->captured_response_buffer);
    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_TRUE(cJSON_IsBool(ok));
    TEST_ASSERT_TRUE(cJSON_IsTrue(ok));
    cJSON_Delete(root);
}

TEST_CASE(
    "provision_rejects_non_json_body [fw-05.4]",
    "[softap][fw-05.4][guard][bite-proof]")
{
    const char *body = "not json at all {{";
    mock_httpd_req_t *req = drive_provision_guard(body, NULL);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    assert_rejection_with_validation_msg(req, "json");

    mock_httpd_req_free(req);
}

TEST_CASE(
    "provision_rejects_missing_wifi_ssid [fw-05.4]",
    "[softap][fw-05.4][guard][bite-proof]")
{
    const char *body =
        "{\"wifi_password\":\"x\","
        "\"name\":\"n\","
        "\"description\":\"d\"}";
    mock_httpd_req_t *req = drive_provision_guard(body, NULL);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    assert_rejection_with_validation_msg(req, "wifi_ssid");

    mock_httpd_req_free(req);
}

TEST_CASE(
    "provision_rejects_missing_wifi_password [fw-05.4]",
    "[softap][fw-05.4][guard][bite-proof]")
{
    const char *body =
        "{\"wifi_ssid\":\"s\","
        "\"name\":\"n\","
        "\"description\":\"d\"}";
    mock_httpd_req_t *req = drive_provision_guard(body, NULL);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* PRD L130: JSON key is wifi_password, error name is "password". */
    assert_rejection_with_validation_msg(req, "password");

    mock_httpd_req_free(req);
}

/* FW-05.3 S2 + FW-05.4 (reconciled): `name` is OPTIONAL. When absent
 * the handler preserves the in-memory cfg->identity.name (which
 * boot_run() loaded from NVS). This test seeds identity.name with a
 * known value, sends a body that omits `name`, and asserts:
 *   - response is 200 + {"ok":true}
 *   - config_save was called (mock_nvs_write_count > 0)
 *   - esp_restart was called exactly once
 *   - NVS still holds the original `name` value (preserved, not
 *     overwritten) */
TEST_CASE(
    "provision_accepts_missing_name [fw-05.4]",
    "[softap][fw-05.4][partial-update][fw-05.3]")
{
    config_t seed;
    memset(&seed, 0, sizeof(seed));
    strncpy(seed.identity.name, "front-door",
            sizeof(seed.identity.name) - 1);
    seed.identity.name[sizeof(seed.identity.name) - 1] = '\0';

    /* Body omits `name`. wifi_ssid + wifi_password + description are
     * present (description is irrelevant to this test). */
    const char *body =
        "{\"wifi_ssid\":\"x\","
        "\"wifi_password\":\"y\","
        "\"description\":\"z\"}";

    mock_httpd_req_t *req = drive_provision_guard(body, &seed);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    assert_success_200_ok(req);

    /* config_save was called (NVS writes > 0). */
    TEST_ASSERT_GREATER_THAN(0, mock_nvs_write_count());
    /* esp_restart called exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());

    /* The NVS `name` key was preserved from the seed. */
    nvs_handle_t h;
    esp_err_t open_err = nvs_open("config", NVS_READONLY, &h);
    TEST_ASSERT_EQUAL_INT(ESP_OK, open_err);
    char buf[64];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, "name", buf, &len);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("front-door", buf);
    nvs_close(h);

    mock_httpd_req_free(req);
}

/* FW-05.3 S2 + FW-05.4 (reconciled): `description` is OPTIONAL. When
 * absent the handler preserves the in-memory cfg->identity.description.
 * This test seeds identity.description with a known value, sends a
 * body that omits `description`, and asserts 200 + save + restart +
 * NVS `description` key preserved. */
TEST_CASE(
    "provision_accepts_missing_description [fw-05.4]",
    "[softap][fw-05.4][partial-update][fw-05.3]")
{
    config_t seed;
    memset(&seed, 0, sizeof(seed));
    strncpy(seed.identity.description, "covers main entrance",
            sizeof(seed.identity.description) - 1);
    seed.identity.description[sizeof(seed.identity.description) - 1] = '\0';

    /* Body omits `description`. wifi_ssid + wifi_password + name are
     * present (name is irrelevant to this test). */
    const char *body =
        "{\"wifi_ssid\":\"x\","
        "\"wifi_password\":\"y\","
        "\"name\":\"a\"}";

    mock_httpd_req_t *req = drive_provision_guard(body, &seed);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    assert_success_200_ok(req);

    TEST_ASSERT_GREATER_THAN(0, mock_nvs_write_count());
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());

    /* The NVS `description` key was preserved from the seed. */
    nvs_handle_t h;
    esp_err_t open_err = nvs_open("config", NVS_READONLY, &h);
    TEST_ASSERT_EQUAL_INT(ESP_OK, open_err);
    char buf[256];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, "description", buf, &len);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("covers main entrance", buf);
    nvs_close(h);

    mock_httpd_req_free(req);
}

TEST_CASE(
    "provision_accepts_well_formed_body [fw-05.4]",
    "[softap][fw-05.4][green]")
{
    const char *body =
        "{\"wifi_ssid\":\"home-2.4\","
        "\"wifi_password\":\"hunter2\","
        "\"name\":\"front-door\","
        "\"description\":\"covers main entrance\"}";
    mock_httpd_req_t *req = drive_provision_guard(body, NULL);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    assert_success_200_ok(req);

    /* esp_restart called exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());

    mock_httpd_req_free(req);
}
