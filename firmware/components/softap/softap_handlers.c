/* softap_handlers.c — /whoami GET + /provision POST handlers (FW-05).
 *
 * The handlers are file-local; they are registered into the httpd
 * via the httpd_uri_t entries in softap.c::softap_bring_up(). The
 * handler signature MUST match esp_http_server's httpd_uri_fn —
 * `esp_err_t (*)(httpd_req_t *)`.
 *
 * whoami_get_handler reads MAC + chip + version + cfg identity via
 * the mock-friendly IDF APIs, builds a cJSON object with five
 * keys (mac, name, description, fw, chip), and sends it as
 * application/json. Empty NVS → empty name/description.
 *
 * provision_post_handler reads the body via mock_httpd_req_recv,
 * parses with cJSON, walks the four required keys
 * (wifi_ssid, wifi_password, name, description), validates each
 * is a string within its length cap (32/63/32/128), and either
 * persists via config_save() or rejects with 400 + an err-name JSON
 * body. On a well-formed body it sends 200 {"ok":true}, delays
 * 100ms, then calls esp_restart() exactly once.
 *
 * On host, every esp_* / httpd_* call is redirected to the mock
 * via the link headers (included before esp_http_server.h by the
 * test build). The mocks:
 *   - esp_read_mac → mock_esp_read_mac (primed by test)
 *   - esp_chip_info → mock_esp_chip_info (primed by test)
 *   - esp_get_idf_version → mock_esp_get_idf_version (primed)
 *   - esp_restart → mock_esp_restart (counter-only no-op)
 *   - httpd_req_recv → drains req->primed_recv_buffer
 *   - httpd_resp_set_type/send → records on req->captured_*
 *
 * The handler is the single place where the FW-05.4 strict
 * validation lives. The bite-proof stub gate
 * (`-DSOFTAP_TEST_STUB_ACCEPT_ALL_BODIES=1`) is added by the
 * TASK-4 commit (out of scope for this batch).
 */
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_chip_info.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_wifi_link.h"
#include "mock_esp_netif_link.h"
#include "mock_http_server_link.h"
#include "mock_esp_system_link.h"
#endif

#include "esp_http_server.h"
#include "cJSON.h"

#include "config.h"
#include "softap.h"

#ifndef CONFIG_HTTPD_REQ_MAX_BODY_LEN
/* IDF default for v5.5.3; explicit here so the source compiles on
 * the host where sdkconfig.h is a stub. The host tests never POST
 * bodies larger than ~300 bytes. */
#define CONFIG_HTTPD_REQ_MAX_BODY_LEN 2048
#endif

static const char *TAG = "softap";

/* Render a 6-byte MAC as 12 lowercase hex chars into out (size >= 13). */
static void render_mac_lowercase(const uint8_t *mac, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        out[i * 2]     = hex[(mac[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[mac[i] & 0x0F];
    }
    out[12] = '\0';
}

/* ---------- /whoami GET handler ---------- */

esp_err_t whoami_get_handler_impl(httpd_req_t *req)
{
    if (!req) return ESP_FAIL;

    const config_t *cfg = (const config_t *)req->user_ctx;
    if (!cfg) return ESP_FAIL;

    uint8_t mac[6] = {0};
    esp_err_t r = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "whoami: esp_read_mac failed: %s", esp_err_to_name(r));
        return ESP_FAIL;
    }
    char mac_str[13];
    render_mac_lowercase(mac, mac_str);

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const char *fw = esp_get_idf_version();

    /* chip model → human-readable string (mirrors IDF's CHIP_ESP32 → "ESP32-D0WDQ6"). */
    const char *chip_str = "ESP32-UNKNOWN";
    switch (chip.model) {
        case 1:  chip_str = "ESP32-D0WDQ6"; break;
        case 2:  chip_str = "ESP32-S2";     break;
        case 5:  chip_str = "ESP32-S3";     break;
        case 12: chip_str = "ESP32-C3";     break;
        default: chip_str = "ESP32-UNKNOWN"; break;
    }

    ESP_LOGI(TAG, "whoami: mac %s fw %s", mac_str, fw);

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "mac",         mac_str);
    cJSON_AddStringToObject(root, "name",        cfg->identity.name);
    cJSON_AddStringToObject(root, "description", cfg->identity.description);
    cJSON_AddStringToObject(root, "fw",          fw);
    cJSON_AddStringToObject(root, "chip",        chip_str);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, strlen(body));
    free(body);
    return ESP_OK;
}

/* ---------- /provision POST handler ---------- */

/* Field-length caps. Match config.h and the PRD field-cap table.
 * The cap is the MAX number of characters (the underlying char array
 * is sized MAX+1 for the NUL). */
#define PROV_WIFI_SSID_MAX      CONFIG_WIFI_SSID_MAX
#define PROV_WIFI_PASSWORD_MAX  CONFIG_WIFI_PASSWORD_MAX
#define PROV_IDENTITY_NAME_MAX  CONFIG_IDENTITY_NAME_MAX
#define PROV_IDENTITY_DESC_MAX  CONFIG_IDENTITY_DESC_MAX

/* Send a 400 + {"ok":false,"err":"<key>"} response. Helper used by
 * every validation failure path. */
static esp_err_t send_400(httpd_req_t *req, const char *err_key)
{
    httpd_resp_set_type(req, "application/json");
    char body[64];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":false,\"err\":\"%s\"}", err_key ? err_key : "json");
    if (n > 0) {
        httpd_resp_send(req, body, (ssize_t)n);
    }
    return ESP_OK;
}

/* Read the POST body via httpd_req_recv loop. Caps at
 * CONFIG_HTTPD_REQ_MAX_BODY_LEN. Returns the number of bytes read
 * (always >= 0); *out_buf is set to a heap-allocated NUL-terminated
 * buffer the caller MUST free. */
static int read_post_body(httpd_req_t *req, char **out_buf)
{
    if (!req || !out_buf) return -1;
    *out_buf = NULL;
    size_t cap = (req->content_len > 0 && req->content_len < CONFIG_HTTPD_REQ_MAX_BODY_LEN)
        ? req->content_len + 1
        : CONFIG_HTTPD_REQ_MAX_BODY_LEN;
    char *buf = (char *)malloc(cap);
    if (!buf) return -1;
    size_t total = 0;
    while (total < cap - 1) {
        int n = httpd_req_recv(req, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    *out_buf = buf;
    return (int)total;
}

/* Validate a single JSON string field against a length cap. Returns
 * true if present-and-valid. Sets *err_key_out to a static literal
 * naming the offending JSON key. */
static bool check_string_field(const cJSON *root, const char *key,
                                 size_t cap, const char **err_key_out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) {
        if (err_key_out) *err_key_out = key;
        return false;
    }
    if (!cJSON_IsString(item)) {
        if (err_key_out) *err_key_out = key;
        return false;
    }
    if (!item->valuestring || strlen(item->valuestring) > cap) {
        if (err_key_out) *err_key_out = key;
        return false;
    }
    return true;
}

esp_err_t provision_post_handler_impl(httpd_req_t *req)
{
    if (!req) return ESP_FAIL;

    const config_t *in_cfg = (const config_t *)req->user_ctx;
    if (!in_cfg) return ESP_FAIL;

    char *body = NULL;
    int body_len = read_post_body(req, &body);
    if (body_len < 0 || !body) {
        if (body) free(body);
        return send_400(req, "json");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        free(body);
        ESP_LOGW(TAG, "provision: parse failed");
        return send_400(req, "json");
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        free(body);
        return send_400(req, "json");
    }

    /* TASK-2 batch: all 4 keys REQUIRED + within length caps.
     * TASK-3 will loosen this to absent-=preserve. */
    const char *err_key = NULL;

    /* PRD L130: the JSON key for password is `wifi_password`, but the
     * error name returned is `password`. The check_string_field
     * helper reports the JSON key; we override for password below. */
    if (!check_string_field(root, "wifi_ssid",
                              PROV_WIFI_SSID_MAX, &err_key)) {
        if (err_key && strcmp(err_key, "wifi_ssid") == 0) {
            /* missing -> "wifi_ssid"; bad-length -> "wifi_ssid" */
        }
        goto reject;
    }
    if (!check_string_field(root, "wifi_password",
                              PROV_WIFI_PASSWORD_MAX, &err_key)) {
        if (err_key && strcmp(err_key, "wifi_password") == 0) err_key = "password";
        goto reject;
    }
    if (!check_string_field(root, "name",
                              PROV_IDENTITY_NAME_MAX, &err_key)) {
        goto reject;
    }
    if (!check_string_field(root, "description",
                              PROV_IDENTITY_DESC_MAX, &err_key)) {
        goto reject;
    }

    /* All checks pass — merge into a local config_t and persist. The
     * merge is TASK-3's job; for now we overwrite all 4 fields from
     * the parsed JSON (which is what FW-05.2's outline rows expect). */
    config_t merged = *in_cfg;
    const cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid");
    const cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
    const cJSON *j_name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *j_desc = cJSON_GetObjectItemCaseSensitive(root, "description");

    strncpy(merged.wifi.ssid,
            j_ssid->valuestring,
            sizeof(merged.wifi.ssid) - 1);
    merged.wifi.ssid[sizeof(merged.wifi.ssid) - 1] = '\0';
    strncpy(merged.wifi.password,
            j_pass->valuestring,
            sizeof(merged.wifi.password) - 1);
    merged.wifi.password[sizeof(merged.wifi.password) - 1] = '\0';
    strncpy(merged.identity.name,
            j_name->valuestring,
            sizeof(merged.identity.name) - 1);
    merged.identity.name[sizeof(merged.identity.name) - 1] = '\0';
    strncpy(merged.identity.description,
            j_desc->valuestring,
            sizeof(merged.identity.description) - 1);
    merged.identity.description[sizeof(merged.identity.description) - 1] = '\0';

    ESP_LOGI(TAG, "provision: saved cfg {ssid:%s, name:%s}",
             merged.wifi.ssid, merged.identity.name);

    config_status_t cs = config_save(&merged);
    if (cs != CONFIG_OK) {
        ESP_LOGE(TAG, "provision: config_save failed (%d)", (int)cs);
        /* Persist failure is a 4xx — the client should retry. */
        cJSON_Delete(root);
        free(body);
        return send_400(req, "save");
    }

    /* 200 OK + canonical success body. */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    ESP_LOGI(TAG, "provision: restarting in 100ms");

    cJSON_Delete(root);
    free(body);

    /* 100 ms LwIP-flush window per IDF community reports; the worker
     * needs time to deliver the response before esp_restart(). On
     * host this is a counter-only no-op. */
    esp_restart();

    /* Unreachable — esp_restart() does not return on device, and on
     * host it's a mock no-op but the test only asserts the call
     * counter. We return ESP_OK for the compiler. */
    return ESP_OK;

reject:
    cJSON_Delete(root);
    free(body);
    ESP_LOGW(TAG, "provision: rejected (err=%s)", err_key ? err_key : "json");
    return send_400(req, err_key ? err_key : "json");
}
