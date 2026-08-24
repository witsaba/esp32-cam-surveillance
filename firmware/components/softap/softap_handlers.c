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
 * application/json. Empty NVS → empty name/description. After a
 * successful /provision, boot_run() reloads cfg from NVS, so the
 * same handler surfaces the new identity on the next /whoami
 * (FW-05.3 S1 round-trip).
 *
 * provision_post_handler reads the body via mock_httpd_req_recv,
 * parses with cJSON, and applies validation + merge. Per PRD §
 * FR-1a L122-131 + milestone FW-05.3 S2 + FW-05.4 S2 (reconciled):
 *
 *   Validation (req-softap-004, relaxed per batch-3 fix):
 *     - Non-JSON body → 400 err="json" (no save, no reboot)
 *     - JSON missing wifi_ssid or wifi_password → 400 err=<key>
 *       (name + description are OPTIONAL; see merge below)
 *     - JSON with any present-and-string field exceeding its
 *       length cap → 400 err=<key>
 *
 *   Merge (req-softap-003 — partial update):
 *     - wifi_ssid + wifi_password: REQUIRED, always written when
 *       the body passes validation.
 *     - name + description: OPTIONAL. Absent from the JSON →
 *       preserve the corresponding cfg field (the value boot_run()
 *       loaded from NVS). Present as a string → overwrite (empty
 *       string present is an explicit clear, not a preserve).
 *
 *   This reconciles FW-05.3 S2 (partial-update preserves NVS
 *   identity when omitted) with FW-05.4 S2 (missing required keys
 *   get a 400). The previous (batch-2) implementation made all 4
 *   keys required, which broke the partial-update scenario.
 *
 * On a well-formed body (wifi_ssid + wifi_password present + within
 * caps; name + description either present-within-caps or absent)
 * the handler persists via config_save(), sends 200 {"ok":true},
 * then calls esp_restart() exactly once.
 *
 * Bite-proof stub gate (FW-05.4):
 *   When the build defines -DSOFTAP_TEST_STUB_ACCEPT_ALL_BODIES=1
 *   (set by run_host_tests.py Pass 4), the validation block is
 *   macro-skipped — the handler proceeds straight to merge + save +
 *   restart regardless of body shape. The Pass-4 runner compiles
 *   only test_softap_guard.c with this flag; under the stub:
 *     - The 3 rejection tests (non-JSON, missing-wifi_ssid,
 *       missing-wifi_password) FAIL because the handler no longer
 *       enforces validation — they assert 400 + no save + no
 *       restart, but the handler now proceeds to 200 + save +
 *       restart. Failure messages contain the literal "validation".
 *     - The 2 accepts-missing-* tests PASS — they assert 200 +
 *       save + restart + preserved identity, which holds under
 *       stub because the merge helper preserves absent keys from
 *       cfg.
 *     - The well-formed test PASSES.
 *   So Pass 4 expects exactly 3 fail + 3 pass.
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
#include "identity.h"
#include "softap.h"

/* Diagnostic /snapshot — pulls one queued frame via the capture
 * queue (single-caller invariant on esp_camera_fb_get stays
 * intact: the handler NEVER calls esp_camera_fb_get). */
#include "capture.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_camera_link.h"
#else
#include "esp_camera.h"
#endif

#ifndef CONFIG_HTTPD_REQ_MAX_BODY_LEN
/* IDF default for v5.5.3; explicit here so the source compiles on
 * the host where sdkconfig.h is a stub. The host tests never POST
 * bodies larger than ~300 bytes. */
#define CONFIG_HTTPD_REQ_MAX_BODY_LEN 2048
#endif

static const char *TAG = "softap";

/* ---------- /whoami GET handler ---------- */

esp_err_t whoami_get_handler_impl(httpd_req_t *req)
{
    if (!req) return ESP_FAIL;

    /* Identity source: provisioning-time registrations pass a live
     * cfg pointer via user_ctx; the STA-interface listener (FW-05.5)
     * registers with user_ctx=NULL BY DESIGN so this handler falls
     * back to reading NVS on every request (re-provisioning visible
     * without a restart). Returning bare ESP_FAIL on NULL closed the
     * connection with zero bytes — every LAN caller saw an empty
     * reply (device-verified 2026-08-24). */
    config_t live_cfg;
    const config_t *cfg = (const config_t *)req->user_ctx;
    if (!cfg) {
        bool live_dirty = false;
        config_status_t st = config_load(&live_cfg, &live_dirty);
        if (st != CONFIG_OK) {
            ESP_LOGE(TAG, "whoami: config_load failed: %d", (int)st);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"error\":\"config_unavailable\"}");
            return ESP_FAIL;
        }
        cfg = &live_cfg;
    }

    uint8_t mac[6] = {0};
    esp_err_t r = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "whoami: esp_read_mac failed: %s", esp_err_to_name(r));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"mac_unavailable\"}");
        return ESP_FAIL;
    }
    char mac_str[13];
    identity_mac_to_hex_lower(mac, mac_str, sizeof(mac_str));

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

/* Apply partial-update semantics for a single string field:
 *   - If `key` is ABSENT from `root` (NULL) → leave `dest` untouched
 *     (preserve the existing in-memory NVS-backed value).
 *   - If `key` is PRESENT as a cJSON string → copy into `dest`,
 *     NUL-terminating at dest_cap - 1 (the empty-string case is a
 *     write, not a preserve — explicit clear semantics).
 *
 * `dest` MUST be a writable char array of size `dest_cap`.
 * This function does NOT call check_string_field; for the REQUIRED
 * fields (wifi_ssid, wifi_password) the validation block has already
 * asserted presence + type + length-cap. For the OPTIONAL fields
 * (name, description) the validation block only checks length-cap
 * WHEN the field is present; the helper handles absent cleanly.
 *
 * FW-05.3 refactor: extracted from the inline merge block in
 * provision_post_handler_impl for clarity (4 fields, identical
 * pattern). Under both production and stub builds the helper is
 * used; under stub, more keys may be absent and the helper still
 * preserves them correctly. */
static void softap_apply_provision_field(const cJSON *root, const char *key,
                                           char *dest, size_t dest_cap)
{
    if (!root || !key || !dest || dest_cap == 0) return;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) return;  /* absent → preserve */
    if (!cJSON_IsString(item) || !item->valuestring) return;  /* defensive */
    /* Length is already validated by check_string_field against the
     * field-specific cap; dest_cap is the same cap. */
    strncpy(dest, item->valuestring, dest_cap - 1);
    dest[dest_cap - 1] = '\0';
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

#ifndef SOFTAP_TEST_STUB_ACCEPT_ALL_BODIES
    /* FW-05.4 guard (relaxed per batch-3 fix). Production + the green
     * build of Pass 1 in run_host_tests.py enforce this block. The
     * stub build (Pass 4, with -DSOFTAP_TEST_STUB_ACCEPT_ALL_BODIES=1)
     * skips the entire block to verify the guard is load-bearing —
     * under stub, the rejection tests in test_softap_guard.c that
     * target REQUIRED keys (non-JSON, missing-wifi_ssid,
     * missing-wifi_password) FAIL with "validation" in the message
     * because the handler no longer returns 400 for those malformed
     * bodies.
     *
     *   REQUIRED (absent → 400):
     *     - wifi_ssid
     *     - wifi_password  (JSON key, error name "password" per PRD L130)
     *
     *   OPTIONAL (absent → preserve from cfg; over-cap → 400):
     *     - name
     *     - description
     *
     * The optional fields use a length check only when the field is
     * present (over-cap present → 400), distinct from check_string_field
     * which treats absent as a failure. */
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

    const char *err_key = NULL;

    /* wifi_ssid — REQUIRED. */
    if (!check_string_field(root, "wifi_ssid",
                              PROV_WIFI_SSID_MAX, &err_key)) {
        goto reject;
    }
    /* wifi_password — REQUIRED (JSON key `wifi_password`, error name
     * `password` per PRD L130). */
    if (!check_string_field(root, "wifi_password",
                              PROV_WIFI_PASSWORD_MAX, &err_key)) {
        if (err_key && strcmp(err_key, "wifi_password") == 0) err_key = "password";
        goto reject;
    }
    /* name — OPTIONAL. Absent → preserved by the merge block below.
     * If present AND over-cap → 400; if present AND within cap →
     * the merge block overwrites it. */
    {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (name) {
            if (!cJSON_IsString(name) || !name->valuestring
                || strlen(name->valuestring) > PROV_IDENTITY_NAME_MAX) {
                err_key = "name";
                goto reject;
            }
        }
    }
    /* description — OPTIONAL. Same shape as `name`. */
    {
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        if (desc) {
            if (!cJSON_IsString(desc) || !desc->valuestring
                || strlen(desc->valuestring) > PROV_IDENTITY_DESC_MAX) {
                err_key = "description";
                goto reject;
            }
        }
    }
#endif /* SOFTAP_TEST_STUB_ACCEPT_ALL_BODIES */

    /* Merge logic — runs in both production and stub builds.
     *
     *   FW-05.3 partial-update semantics: for each of the 4 keys,
     *     - absent (NULL) → preserve the existing cfg field (this is
     *       the genuine partial-update path now that the validation
     *       block no longer rejects missing name/description)
     *     - present as string → overwrite the cfg field (including
     *       "" to explicitly clear a field)
     *
     *   Under the production guard, only wifi_ssid + wifi_password
     *   are guaranteed present; name + description may be absent and
     *   are therefore preserved from cfg (FW-05.3 S2). Under the stub
     *   build, all 4 keys may be absent and all 4 get preserved.
     */
    config_t merged = *in_cfg;
    softap_apply_provision_field(root, "wifi_ssid",
                                  merged.wifi.ssid,
                                  sizeof(merged.wifi.ssid));
    softap_apply_provision_field(root, "wifi_password",
                                  merged.wifi.password,
                                  sizeof(merged.wifi.password));
    softap_apply_provision_field(root, "name",
                                  merged.identity.name,
                                  sizeof(merged.identity.name));
    softap_apply_provision_field(root, "description",
                                  merged.identity.description,
                                  sizeof(merged.identity.description));

    ESP_LOGI(TAG, "provision: saved cfg {ssid:%s, name:%s}",
             merged.wifi.ssid, merged.identity.name);

    config_status_t cs = config_save(&merged);
    if (cs != CONFIG_OK) {
        ESP_LOGE(TAG, "provision: config_save failed (%d)", (int)cs);
        /* Persist failure is a 4xx — the client should retry. */
        if (root) cJSON_Delete(root);
        free(body);
        return send_400(req, "save");
    }

    /* 200 OK + canonical success body. */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    ESP_LOGI(TAG, "provision: restarting in 100ms");

    if (root) cJSON_Delete(root);
    free(body);

    /* 100 ms LwIP-flush window per IDF community reports; the worker
     * needs time to deliver the response before esp_restart(). On
     * host this is a counter-only no-op. */
    esp_restart();

    /* Unreachable — esp_restart() does not return on device, and on
     * host it's a mock no-op but the test only asserts the call
     * counter. We return ESP_OK for the compiler. */
    return ESP_OK;

#ifndef SOFTAP_TEST_STUB_ACCEPT_ALL_BODIES
reject:
    if (root) cJSON_Delete(root);
    free(body);
    ESP_LOGW(TAG, "provision: rejected (err=%s)", err_key ? err_key : "json");
    return send_400(req, err_key ? err_key : "json");
#endif
}

/* ---------- diagnostic GET /snapshot handler ----------
 *
 * Serves ONE real JPEG frame per request. The frame comes from the
 * depth-2 capture queue (capture_queue_receive_timeout), NOT from
 * esp_camera_fb_get — R-16/FW-11.3 keep the capture task as the
 * sole driver caller. Consumer-owned buffer: fb_return fires after
 * the response bytes are handed to httpd, mirroring the FW-15
 * stream-task ownership contract.
 *
 * Bisect semantics:
 *   200 image/jpeg  — sensor + driver + queue alive; streaming
 *                     failures live downstream (WS/network).
 *   503 no_frame    — nothing produced within the bounded wait;
 *                     camera hardware is not delivering frames,
 *                     independent of any transport.
 */
esp_err_t snapshot_get_handler_impl(httpd_req_t *req)
{
    /* 2 s bounded wait: a healthy QVGA@5fps pipeline produces every
     * ~200 ms, so 2 s is generous without wedging the httpd worker. */
    const uint32_t SNAPSHOT_FRAME_WAIT_MS = 2000;

    void *p = NULL;
    if (!capture_queue_receive_timeout(&p, SNAPSHOT_FRAME_WAIT_MS) ||
        p == NULL) {
        ESP_LOGW(TAG, "snapshot: no frame within %u ms",
                 (unsigned)SNAPSHOT_FRAME_WAIT_MS);
        httpd_resp_set_status(req, "503 Camera Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"no_frame\"}");
        return ESP_FAIL;
    }

    camera_fb_t *fb = (camera_fb_t *)p;
    ESP_LOGI(TAG, "snapshot: serving %u bytes", (unsigned)fb->len);
    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t r = httpd_resp_send(req, (const char *)fb->buf,
                                  (ssize_t)fb->len);
    esp_camera_fb_return(fb);
    return r;
}
