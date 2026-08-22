/* test_softap_home.c — FW-05 home page handler tests (scope expansion 2026-08-22).
 *
 * Tests the GET / handler implemented in softap_home.c. The handler
 * serves a minimal HTML form that POSTs to /provision. Without this,
 * a phone user connecting to the softAP has no way to issue the
 * POST (no developer tools on a phone).
 *
 * Assertions per test:
 *   - Status 200
 *   - Content-type starts with "text/html"
 *   - Body contains the 4 form input names (wifi_ssid, wifi_password,
 *     name, description)
 *   - Body contains the rendered MAC address (lowercase hex)
 *   - Body contains the action URL (/provision)
 *
 * We don't full-parse the HTML — that's brittle for a minimal test
 * surface. The assertions above are sufficient to prove the page
 * will render correctly in a browser.
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

/* Seed the system mocks for the home page. The home handler reads
 * MAC via esp_read_mac + IDENTITY via cfg->identity.{name,description}. */
static void seed_home_mocks(void)
{
    const uint8_t mac[6] = {0xC8, 0xF0, 0x9E, 0x9D, 0x50, 0x08};
    mock_esp_read_mac_set_bytes(mac);
}

TEST_CASE(
    "home_get_serves_html_form_with_provision_action [fw-05][home-page]",
    "[softap][fw-05][home-page]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_esp_wifi_reset();
    mock_esp_netif_reset();
    mock_esp_event_reset();
    mock_httpd_reset();
    mock_log_reset();
    seed_home_mocks();

    /* Seed cfg with empty identity to test the fresh-device render. */
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;

    /* Bring up the softAP component (registers /, /whoami, /provision). */
    boot_status_t s = softap_run_provisioning(&cfg);
    (void)s;

    /* Drive the GET / handler. */
    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);

    esp_err_t rc = mock_httpd_invoke_registered_handler("/", 0 /*HTTP_GET*/, req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    /* Status + content-type. */
    TEST_ASSERT_EQUAL_INT(200, req->captured_status);
    TEST_ASSERT_NOT_NULL(req->captured_content_type);
    TEST_ASSERT_NOT_NULL(strstr(req->captured_content_type, "text/html"));

    /* Body must exist + contain the form action URL + the 4 input names. */
    TEST_ASSERT_NOT_NULL(req->captured_response_buffer);
    TEST_ASSERT_GREATER_THAN(100, req->captured_response_len);
    const char *body = req->captured_response_buffer;
    /* The page is ~2.1 KB. strstr should find /provision in the JS
     * fetch('/provision', ...). */
    const char *hit = strstr(body, "/provision");
    TEST_ASSERT_NOT_NULL_MESSAGE(hit, "body does not contain '/provision'");
    TEST_ASSERT_NOT_NULL(strstr(body, "wifi_ssid"));
    TEST_ASSERT_NOT_NULL(strstr(body, "wifi_ssid"));
    TEST_ASSERT_NOT_NULL(strstr(body, "wifi_password"));
    TEST_ASSERT_NOT_NULL(strstr(body, "name"));
    TEST_ASSERT_NOT_NULL(strstr(body, "description"));

    /* Rendered MAC (lowercase 12-char hex). */
    TEST_ASSERT_NOT_NULL(strstr(body, "c8f09e9d5008"));

    /* Some sanity on the surrounding markup. */
    TEST_ASSERT_NOT_NULL(strstr(body, "<form"));
    TEST_ASSERT_NOT_NULL(strstr(body, "<input"));
    TEST_ASSERT_NOT_NULL(strstr(body, "<button"));

    mock_httpd_req_free(req);
}

/* Re-provisioning: when cfg has existing identity values, the home
 * page pre-fills them so the user doesn't have to retype. Per FW-05.3
 * S1 round-trip semantics. */
TEST_CASE(
    "home_get_prefills_existing_identity [fw-05][home-page][fw-05.3]",
    "[softap][fw-05][home-page][fw-05.3]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_esp_wifi_reset();
    mock_esp_netif_reset();
    mock_esp_event_reset();
    mock_httpd_reset();
    mock_log_reset();
    seed_home_mocks();

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    strncpy(cfg.identity.name, "front-door", sizeof(cfg.identity.name) - 1);
    strncpy(cfg.identity.description, "covers main entrance", sizeof(cfg.identity.description) - 1);

    boot_status_t s = softap_run_provisioning(&cfg);
    (void)s;

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);
    esp_err_t rc = mock_httpd_invoke_registered_handler("/", 0, req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    const char *body = req->captured_response_buffer;
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_NOT_NULL(strstr(body, "front-door"));
    TEST_ASSERT_NOT_NULL(strstr(body, "covers main entrance"));

    mock_httpd_req_free(req);
}

/* HTML escaping: an identity string with `<` must render as `&lt;`
 * in the HTML value, so a malicious or accidental NVS-stored name
 * can't inject script. */
TEST_CASE(
    "home_get_html_escapes_identity [fw-05][home-page][security]",
    "[softap][fw-05][home-page][security]")
{
    mock_nvs_reset();
    mock_esp_system_reset();
    mock_esp_wifi_reset();
    mock_esp_netif_reset();
    mock_esp_event_reset();
    mock_httpd_reset();
    mock_log_reset();
    seed_home_mocks();

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = CONFIG_SCHEMA_VERSION;
    /* The classic XSS test payload. */
    strncpy(cfg.identity.name, "<script>alert(1)</script>", sizeof(cfg.identity.name) - 1);

    boot_status_t s = softap_run_provisioning(&cfg);
    (void)s;

    mock_httpd_req_t *req = mock_httpd_req_new();
    TEST_ASSERT_NOT_NULL(req);
    esp_err_t rc = mock_httpd_invoke_registered_handler("/", 0, req);
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    const char *body = req->captured_response_buffer;
    TEST_ASSERT_NOT_NULL(body);
    /* The raw XSS payload must NOT appear in the value="..." attribute
     * for the Name input. We check for value="<script> specifically —
     * the un-escaped payload would put the malicious tag in an HTML
     * attribute that browsers execute. The escaped form &lt;script&gt;
     * is what we expect (it's just text inside the value="..."). */
    TEST_ASSERT_NULL_MESSAGE(strstr(body, "value=\"<script>"),
        "raw <script> in value attribute should be HTML-escaped to &lt;script&gt;");
    TEST_ASSERT_NOT_NULL(strstr(body, "value=\"&lt;script&gt;alert(1)&lt;/script&gt;\""));

    mock_httpd_req_free(req);
}
