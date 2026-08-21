/* test_softap_whoami.c — FW-05.1 GET /whoami + FW-05.3 round-trip.
 *
 * FW-05.1 (R-10): fresh device's /whoami returns the canonical
 * identity JSON: {mac, name, description, fw, chip}. Content-Type
 * is application/json; body parses via cJSON_Parse as an object;
 * mac is 12-char lowercase hex.
 *
 * FW-05.3 S1 (R-12): re-provisioning round-trip — cfg seeded with
 * name="front-door" / description="covers main entrance" still
 * surfaces those values through /whoami.
 *
 * The handler is invoked directly via
 * `mock_httpd_invoke_registered_handler(uri, method, req)` so we
 * bypass the httpd worker thread. The request body for /whoami is
 * NULL — the GET path does not call mock_httpd_req_recv.
 *
 * Mocks used:
 *   - mock_esp_read_mac (primed by test)
 *   - mock_esp_chip_info (primed by test)
 *   - mock_esp_get_idf_version (primed by test)
 *   - mock_httpd_invoke_registered_handler / mock_httpd_req_*
 *   - cfg identity fields are read directly from the seed
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

/* Seed identity JSON mocks with the canonical fresh-device values. */
static void seed_whoami_mocks(void)
{
    const uint8_t mac[6] = {0xC8, 0xF0, 0x9E, 0x9D, 0x50, 0x08};
    mock_esp_read_mac_set_bytes(mac);
    mock_esp_chip_info_set(/*CHIP_ESP32*/ 1, /*revision*/ 3);
    mock_esp_get_idf_version_set("v5.5.3");
}

TEST_CASE(
    "whoami_returns_identity_json_fresh_device [fw-05.1]",
    "[softap][fw-05.1]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_httpd_reset();
    mock_log_reset();
    seed_whoami_mocks();

    /* Pre-seed NVS to model "fresh device" — schema_version present,
     * every field empty. config_load() would populate cfg; we model
     * the post-load state directly so the test does not depend on
     * boot_run() flow. */
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    /* identity.name, identity.description already zero -> empty strings */

    /* Bring up the softAP component. This registers the /whoami URI
     * handler with mock_http_server. The function returns when
     * esp_restart() would normally fire — but the mock's
     * mock_esp_restart is a no-op, so the bring-up blocks at
     * httpd_start() (also a mock no-op). On host we expect the
     * bring-up to "complete" without registering errors. */
    boot_status_t s = softap_run_provisioning(&cfg);
    /* On the mock path every step is ESP_OK, so the function returns
     * with the sentinel status set by softap.c. We don't assert the
     * return value here — we only care about the registered handler. */
    (void)s;

    /* Drive the /whoami handler with a fresh request. */
    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);

    /* Invoke the registered /whoami GET handler. */
    esp_err_t rc = mock_httpd_invoke_registered_handler("/whoami",
                                                        0 /*HTTP_GET*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* The handler must have produced an application/json response. */
    TEST_ASSERT_NOT_NULL(req->captured_response_buffer);

    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_TRUE(cJSON_IsObject(root));

    /* Five keys must be present. */
    cJSON *mac_item     = cJSON_GetObjectItemCaseSensitive(root, "mac");
    cJSON *name_item    = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *desc_item    = cJSON_GetObjectItemCaseSensitive(root, "description");
    cJSON *fw_item      = cJSON_GetObjectItemCaseSensitive(root, "fw");
    cJSON *chip_item    = cJSON_GetObjectItemCaseSensitive(root, "chip");
    TEST_ASSERT_NOT_NULL(mac_item);
    TEST_ASSERT_NOT_NULL(name_item);
    TEST_ASSERT_NOT_NULL(desc_item);
    TEST_ASSERT_NOT_NULL(fw_item);
    TEST_ASSERT_NOT_NULL(chip_item);

    TEST_ASSERT_TRUE(cJSON_IsString(mac_item));
    TEST_ASSERT_TRUE(cJSON_IsString(name_item));
    TEST_ASSERT_TRUE(cJSON_IsString(desc_item));
    TEST_ASSERT_TRUE(cJSON_IsString(fw_item));
    TEST_ASSERT_TRUE(cJSON_IsString(chip_item));

    /* Fresh device: name + description are empty strings. */
    TEST_ASSERT_EQUAL_STRING("", name_item->valuestring);
    TEST_ASSERT_EQUAL_STRING("", desc_item->valuestring);

    /* MAC rendered as 12 lowercase hex. */
    TEST_ASSERT_EQUAL_STRING("c8f09e9d5008", mac_item->valuestring);

    /* fw = "v5.5.3", chip = "ESP32-D0WDQ6". */
    TEST_ASSERT_EQUAL_STRING("v5.5.3", fw_item->valuestring);
    TEST_ASSERT_EQUAL_STRING("ESP32-D0WDQ6", chip_item->valuestring);

    cJSON_Delete(root);
    mock_httpd_req_free(req);
}

TEST_CASE(
    "whoami_response_is_application_json_and_parses [fw-05.1]",
    "[softap][fw-05.1]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_httpd_reset();
    mock_log_reset();
    seed_whoami_mocks();

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;

    boot_status_t s = softap_run_provisioning(&cfg);
    (void)s;

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/whoami",
                                                        0 /*HTTP_GET*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Content-Type must be application/json — recorded on the req. */
    TEST_ASSERT_NOT_NULL(req->captured_content_type);
    TEST_ASSERT_EQUAL_STRING("application/json", req->captured_content_type);

    /* Body parses as a cJSON object (not array, not string, not null). */
    TEST_ASSERT_NOT_NULL(req->captured_response_buffer);
    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_TRUE(cJSON_IsObject(root));

    cJSON_Delete(root);
    mock_httpd_req_free(req);
}

TEST_CASE(
    "whoami_mac_is_12_char_lowercase_hex [fw-05.1]",
    "[softap][fw-05.1]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_httpd_reset();
    mock_log_reset();

    /* Different MAC bytes than the other two tests — proves the
     * handler reads live MAC bytes, not a constant. */
    const uint8_t mac[6] = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56};
    mock_esp_read_mac_set_bytes(mac);
    mock_esp_chip_info_set(1, 3);
    mock_esp_get_idf_version_set("v5.5.3");

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;

    boot_status_t s = softap_run_provisioning(&cfg);
    (void)s;

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/whoami",
                                                        0 /*HTTP_GET*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *mac_item = cJSON_GetObjectItemCaseSensitive(root, "mac");
    TEST_ASSERT_NOT_NULL(mac_item);
    TEST_ASSERT_TRUE(cJSON_IsString(mac_item));

    /* Expect "abcdef123456" — 12 lowercase hex chars, no 0x prefix. */
    const char *got = mac_item->valuestring;
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(12, (int)strlen(got));
    TEST_ASSERT_EQUAL_STRING("abcdef123456", got);
    for (int i = 0; i < 12; ++i) {
        char c = got[i];
        TEST_ASSERT_TRUE_MESSAGE(
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
            "mac field must be lowercase hex");
    }

    cJSON_Delete(root);
    mock_httpd_req_free(req);
}

/* FW-05.3 S1 (R-12): pre-provisioned device — /whoami reflects the
 * existing in-memory cfg.identity.{name,description}. Proves the
 * /whoami handler reads from the cfg pointer passed via user_ctx on
 * every invocation (not from a stale local copy).
 *
 * boot_run() loads cfg from NVS at boot, so a previously-saved config
 * is what the handler should surface. We model that here by seeding
 * cfg.identity directly before invoking the handler. */
TEST_CASE(
    "whoami_round_trips_existing_name_and_description [fw-05.3]",
    "[softap][fw-05.3][reprovision]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_httpd_reset();
    mock_log_reset();
    seed_whoami_mocks();

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    /* Seed name + description as if a prior /provision had succeeded
     * (or as if boot_run() had loaded them from NVS). */
    strncpy(cfg.identity.name,
            "front-door",
            sizeof(cfg.identity.name) - 1);
    cfg.identity.name[sizeof(cfg.identity.name) - 1] = '\0';
    strncpy(cfg.identity.description,
            "covers main entrance",
            sizeof(cfg.identity.description) - 1);
    cfg.identity.description[sizeof(cfg.identity.description) - 1] = '\0';

    boot_status_t s = softap_run_provisioning(&cfg);
    (void)s;

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/whoami",
                                                        0 /*HTTP_GET*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    TEST_ASSERT_NOT_NULL(req->captured_response_buffer);
    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_TRUE(cJSON_IsObject(root));

    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *desc_item = cJSON_GetObjectItemCaseSensitive(root, "description");
    TEST_ASSERT_NOT_NULL(name_item);
    TEST_ASSERT_NOT_NULL(desc_item);
    TEST_ASSERT_TRUE(cJSON_IsString(name_item));
    TEST_ASSERT_TRUE(cJSON_IsString(desc_item));

    /* The crucial assertion: the seeded values are surfaced (not
     * empty strings, which would indicate the handler was reading
     * a fresh cfg instead of the user_ctx). */
    TEST_ASSERT_EQUAL_STRING("front-door", name_item->valuestring);
    TEST_ASSERT_EQUAL_STRING("covers main entrance", desc_item->valuestring);

    /* mac/fw/chip still populated — verify no regression on the
     * FW-05.1 fields. */
    cJSON *mac_item = cJSON_GetObjectItemCaseSensitive(root, "mac");
    cJSON *fw_item  = cJSON_GetObjectItemCaseSensitive(root, "fw");
    cJSON *chip_item = cJSON_GetObjectItemCaseSensitive(root, "chip");
    TEST_ASSERT_NOT_NULL(mac_item);
    TEST_ASSERT_NOT_NULL(fw_item);
    TEST_ASSERT_NOT_NULL(chip_item);
    TEST_ASSERT_EQUAL_STRING("c8f09e9d5008", mac_item->valuestring);
    TEST_ASSERT_EQUAL_STRING("v5.5.3", fw_item->valuestring);
    TEST_ASSERT_EQUAL_STRING("ESP32-D0WDQ6", chip_item->valuestring);

    cJSON_Delete(root);
    mock_httpd_req_free(req);
}
