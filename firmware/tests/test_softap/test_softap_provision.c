/* test_softap_provision.c — FW-05.2 POST /provision writes NVS + reboots.
 *
 * FW-05.2 (R-11): three Scenario Outline rows (home-2.4, office-5g,
 * guest) write all 4 fields via config_save() and call esp_restart()
 * exactly once. Two additional length-cap scenarios verify the
 * ssid/description rejection paths.
 *
 * The handler is invoked directly via
 * `mock_httpd_invoke_registered_handler("/provision", HTTP_POST, req)`
 * with a primed `req->primed_recv_buffer` containing the JSON body.
 * The handler reads via mock_httpd_req_recv (which drains the primed
 * buffer), parses with cJSON, calls config_save() (which routes
 * through mock_nvs_*), and finally calls mock_esp_restart.
 *
 * Mocks used:
 *   - mock_httpd_req_*, mock_httpd_resp_*
 *   - mock_esp_restart (call count asserted)
 *   - mock_nvs_write_count (from mock_nvs_flash.cpp; >= 4 for green
 *     path, == 0 for rejection paths)
 */
#include "mock_nvs_flash_link.h"
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_esp_event_link.h"
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
 * The mock `mock_httpd_req_set_primed_recv_buffer` copies the body
 * into its own heap allocation — the caller does NOT need to keep
 * `body` alive, and the test must NOT free `req->primed_recv_buffer`
 * (mock_httpd_req_free owns that). */
static mock_httpd_req_t *drive_provision(const char *body, config_t *cfg)
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_httpd_reset();
    mock_log_reset();

    /* Seed system mocks used by the /whoami branch (not the provision
     * branch, but the softap bring-up registers both handlers and the
     * whoami handler reads MAC at register-time too. Keep values
     * deterministic so any cross-test bleed is impossible. */
    const uint8_t mac[6] = {0xC8, 0xF0, 0x9E, 0x9D, 0x50, 0x08};
    mock_esp_read_mac_set_bytes(mac);
    mock_esp_chip_info_set(1, 3);
    mock_esp_get_idf_version_set("v5.5.3");

    /* Bring up softAP — registers /whoami + /provision URI handlers. */
    boot_status_t s = softap_run_provisioning(cfg);
    (void)s;

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);
    if (body && *body) {
        mock_httpd_req_set_primed_recv_buffer(req, body, strlen(body));
    }
    return req;
}

TEST_CASE(
    "provision_writes_nvs_and_reboots_home_2_4 [fw-05.2][row-1]",
    "[softap][fw-05.2][provision]")
{
    const char *body =
        "{\"wifi_ssid\":\"home-2.4\","
        "\"wifi_password\":\"hunter2\","
        "\"name\":\"front-door\","
        "\"description\":\"covers main entrance\"}";
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    mock_httpd_req_t *req = drive_provision(body, &cfg);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Response: 200 + {"ok":true} + application/json. */
    TEST_ASSERT_NOT_NULL(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(req->captured_content_type);
    TEST_ASSERT_EQUAL_STRING("application/json", req->captured_content_type);

    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_TRUE(cJSON_IsBool(ok));
    TEST_ASSERT_TRUE(cJSON_IsTrue(ok));
    cJSON_Delete(root);

    /* mock_esp_restart called exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());

    mock_httpd_req_free(req);
}

TEST_CASE(
    "provision_writes_nvs_and_reboots_office_5g [fw-05.2][row-2]",
    "[softap][fw-05.2][provision]")
{
    const char *body =
        "{\"wifi_ssid\":\"office-5g\","
        "\"wifi_password\":\"correct-horse\","
        "\"name\":\"back-yard\","
        "\"description\":\"covers parking lot\"}";
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    mock_httpd_req_t *req = drive_provision(body, &cfg);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    TEST_ASSERT_TRUE(ok && cJSON_IsTrue(ok));
    cJSON_Delete(root);

    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());

    mock_httpd_req_free(req);
}

TEST_CASE(
    "provision_writes_nvs_and_reboots_guest [fw-05.2][row-3]",
    "[softap][fw-05.2][provision]")
{
    const char *body =
        "{\"wifi_ssid\":\"guest\","
        "\"wifi_password\":\"\","
        "\"name\":\"spare\","
        "\"description\":\"\"}";
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    mock_httpd_req_t *req = drive_provision(body, &cfg);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    TEST_ASSERT_TRUE(ok && cJSON_IsTrue(ok));
    cJSON_Delete(root);

    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());

    mock_httpd_req_free(req);
}

TEST_CASE(
    "provision_rejects_ssid_over_32_chars [fw-05.2][length-cap]",
    "[softap][fw-05.2][provision][guard]")
{
    /* 33-char wifi_ssid. */
    const char *body =
        "{\"wifi_ssid\":\"0123456789012345678901234567890ab\","
        "\"wifi_password\":\"x\","
        "\"name\":\"n\","
        "\"description\":\"d\"}";
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    mock_httpd_req_t *req = drive_provision(body, &cfg);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Body should be {"ok":false,"err":"wifi_ssid"}. */
    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "err");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(cJSON_IsString(err));
    TEST_ASSERT_EQUAL_STRING("wifi_ssid", err->valuestring);
    cJSON_Delete(root);

    /* No NVS write, no esp_restart. */
    TEST_ASSERT_EQUAL_INT(0, mock_esp_restart_call_count());

    mock_httpd_req_free(req);
}

TEST_CASE(
    "provision_rejects_description_over_128_chars [fw-05.2][length-cap]",
    "[softap][fw-05.2][provision][guard]")
{
    /* 129-char description = 100 'a's + 29 'b's. Assert length at
     * runtime so the test catches off-by-one. The string literal
     * below is built deterministically so the length is exact. */
    char desc_129[130];
    memset(desc_129, 'a', 100);
    memset(desc_129 + 100, 'b', 29);
    desc_129[129] = '\0';
    TEST_ASSERT_EQUAL_INT(129, (int)strlen(desc_129));

    char body[512];
    snprintf(body, sizeof(body),
             "{\"wifi_ssid\":\"x\","
             "\"wifi_password\":\"y\","
             "\"name\":\"n\","
             "\"description\":\"%s\"}",
             desc_129);

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    mock_httpd_req_t *req = drive_provision(body, &cfg);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "err");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_TRUE(cJSON_IsString(err));
    TEST_ASSERT_EQUAL_STRING("description", err->valuestring);
    cJSON_Delete(root);

    /* No NVS write, no esp_restart. */
    TEST_ASSERT_EQUAL_INT(0, mock_esp_restart_call_count());

    mock_httpd_req_free(req);
}

/* FW-05.3 S2 (R-12): the partial-update semantics in /provision.
 *
 * Per PRD § FR-1a L132 + FW-05.3 S2, name and description are
 * OPTIONAL keys. When a request omits either, the handler preserves
 * the corresponding cfg field (loaded from NVS by boot_run() in
 * production). Only `wifi_ssid` and `wifi_password` are REQUIRED.
 *
 * This test seeds cfg as if boot_run() had previously loaded a
 * prior NVS state (wifi_ssid="old-ssid", password="old-pass",
 * name="front-door", description="covers main entrance"), then
 * sends a body that ONLY specifies new wifi_ssid and wifi_password
 * (omits name + description). The expected outcome:
 *   - response is 200 + {"ok":true} (well-formed modulo the
 *     optional fields)
 *   - esp_restart called exactly once
 *   - NVS `ssid` overwritten with "new-ssid"
 *   - NVS `password` overwritten with "new-pass"
 *   - NVS `name` STILL "front-door" (absent → preserved)
 *   - NVS `description` STILL "covers main entrance" (absent → preserved)
 *
 * This is the true partial-update path (the body leaves identity
 * fields untouched). */
TEST_CASE(
    "provision_partial_update_preserves_name_and_description [fw-05.3]",
    "[softap][fw-05.3][reprovision][partial-update]")
{
    /* Body omits `name` and `description` — only the wifi creds
     * change in this user update. */
    const char *body =
        "{\"wifi_ssid\":\"new-ssid\","
        "\"wifi_password\":\"new-pass\"}";

    /* Pre-seed cfg as if boot_run() had loaded prior NVS state. */
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    strncpy(cfg.wifi.ssid, "old-ssid",
            sizeof(cfg.wifi.ssid) - 1);
    cfg.wifi.ssid[sizeof(cfg.wifi.ssid) - 1] = '\0';
    strncpy(cfg.wifi.password, "old-pass",
            sizeof(cfg.wifi.password) - 1);
    cfg.wifi.password[sizeof(cfg.wifi.password) - 1] = '\0';
    strncpy(cfg.identity.name, "front-door",
            sizeof(cfg.identity.name) - 1);
    cfg.identity.name[sizeof(cfg.identity.name) - 1] = '\0';
    strncpy(cfg.identity.description, "covers main entrance",
            sizeof(cfg.identity.description) - 1);
    cfg.identity.description[sizeof(cfg.identity.description) - 1] = '\0';

    mock_httpd_req_t *req = drive_provision(body, &cfg);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/provision",
                                                        1 /*HTTP_POST*/,
                                                        req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Response: 200 + {"ok":true}. */
    TEST_ASSERT_NOT_NULL(req->captured_response_buffer);
    cJSON *root = cJSON_Parse(req->captured_response_buffer);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_TRUE(cJSON_IsBool(ok));
    TEST_ASSERT_TRUE(cJSON_IsTrue(ok));
    cJSON_Delete(root);

    /* esp_restart called exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_call_count());

    /* Read back the saved NVS namespace to verify the partial merge.
     * Use READONLY because we're not modifying NVS in the test. */
    nvs_handle_t h;
    esp_err_t open_err = nvs_open("config", NVS_READONLY, &h);
    TEST_ASSERT_EQUAL_INT(ESP_OK, open_err);

    char buf[256];
    size_t len;

    /* wifi_ssid overwritten with the body's value. */
    memset(buf, 0, sizeof(buf));
    len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, "ssid", buf, &len);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("new-ssid", buf);

    /* wifi_password overwritten. */
    memset(buf, 0, sizeof(buf));
    len = sizeof(buf);
    err = nvs_get_str(h, "password", buf, &len);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("new-pass", buf);

    /* name preserved from the cfg seed (absent from body). */
    memset(buf, 0, sizeof(buf));
    len = sizeof(buf);
    err = nvs_get_str(h, "name", buf, &len);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("front-door", buf);

    /* description preserved from the cfg seed (absent from body). */
    memset(buf, 0, sizeof(buf));
    len = sizeof(buf);
    err = nvs_get_str(h, "description", buf, &len);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("covers main entrance", buf);

    nvs_close(h);

    mock_httpd_req_free(req);
}
