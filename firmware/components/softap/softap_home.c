/* softap_home.c — minimal provisioning home page (FW-05 home-page scope expansion).
 *
 * Why this exists: the PRD's milestones doc L472 explicitly deferred
 * "captive-portal DNS rebinding for automatic browser redirect" and
 * the PRD L89 deferred "captive portal (covered in a follow-up task;
 * out of scope for the first cut)". The user's directive on
 * 2026-08-22 expanded FW-05 to ship a MINIMAL home page so a phone
 * user can provision the device without developer tools: connect to
 * the softAP, navigate to http://192.168.4.1/, fill the form, hit
 * submit, device reboots.
 *
 * What it does NOT do (deferred to future work, NOT in this PR):
 *   - Captive-portal DNS rebinding (iOS/Android auto-open)
 *   - mDNS / DNS-SD advertisement
 *   - WPA2 PSK on the softAP (stays open per PRD R-26)
 *   - Pre-filling the SSID from a scanned network list
 *
 * The HTML is intentionally tiny — embedded as a C string literal
 * so the firmware carries zero extra files. ~1.4 KB of source, ~1.4 KB
 * of binary. No JavaScript dependencies; the form posts via plain
 * HTTP. Auto-submits via `fetch()` on form submit, then shows the
 * "configured" message. The device reboots ~100ms after the response
 * (per softap_handlers.c provision_post_handler), so the user sees
 * the success message briefly before the connection drops.
 *
 * The handler reads cfg from user_ctx so the MAC is injected into
 * the rendered HTML (so the user can confirm which device they're
 * configuring). Identity pre-fill is for show only; the POST
 * handler validates + writes regardless.
 *
 * On host tests: the HTML is treated as an opaque byte string —
 * tests assert (a) status 200, (b) content-type text/html, (c) body
 * contains the 4 input names (wifi_ssid, wifi_password, name,
 * description), (d) body contains the rendered MAC. No full HTML
 * parse — too brittle for a minimal test surface.
 */
#include "softap.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"      /* esp_read_mac; not auto-included by esp_system.h in IDF v5.5.3 */
#include "esp_heap_caps.h" /* heap_caps_malloc for off-stack page buffer */
#include "esp_http_server.h"

#ifdef UNITY_HOST_BUILD
#include "mock_http_server_link.h"
#include "mock_esp_system_link.h"
#endif

#include "config.h"
#include "identity.h"

static const char *TAG = "softap_home";

/* Helper: format the 6-byte MAC as a 12-char lowercase hex string.
 * Reused by softap_handlers.c::whoami_get_handler_impl but duplicated
 * here to avoid exposing it across modules (keep FW-05 modules
 * self-contained). If a third caller appears, lift to softap_util.c.
  *
 * IMPORTANT: this helper runs on the httpd worker task whose stack
 * is 4096 bytes (hardcoded in IDF v5.5.3 — not Kconfig-tunable).
 * The handler must keep its stack footprint small. The MAC hex
 * string (13 bytes) is the biggest stack local this helper needs.
 * The hex-encoding is delegated to identity_mac_to_hex_lower()
 * (FW-13 identity shared module) — single source of truth for
 * MAC → lower-hex conversion across the firmware. */

/* Build the home page body into `out` (must be ≥ 1024 bytes).
 * `identity_name` and `identity_description` come from the in-memory
 * cfg — they pre-fill the form so the user sees the existing values
 * for re-provisioning (FW-05.3 S1 round-trip semantics). */
static void render_home_page(char *out, size_t cap,
                             const char *mac_hex12,
                             const char *identity_name,
                             const char *identity_description)
{
    /* HTML escaping: only `&`, `<`, `>` matter for the two identity
     * strings; the rest of the page is a static template with no
     * user-controlled data. Worst case: a NVS-stored Name with
     * "<script>" would render as literal text, not as a script tag. */
    char esc_name[64];
    char esc_desc[160];
    size_t ni = 0, di = 0;
    const char *src;

    for (src = identity_name; *src && ni < sizeof(esc_name) - 5; src++) {
        if (*src == '<')      { memcpy(&esc_name[ni], "&lt;",   4); ni += 4; }
        else if (*src == '>') { memcpy(&esc_name[ni], "&gt;",   4); ni += 4; }
        else if (*src == '&') { memcpy(&esc_name[ni], "&amp;",  5); ni += 5; }
        else                  { esc_name[ni++] = *src; }
    }
    esc_name[ni] = '\0';

    for (src = identity_description; *src && di < sizeof(esc_desc) - 5; src++) {
        if (*src == '<')      { memcpy(&esc_desc[di], "&lt;",   4); di += 4; }
        else if (*src == '>') { memcpy(&esc_desc[di], "&gt;",   4); di += 4; }
        else if (*src == '&') { memcpy(&esc_desc[di], "&amp;",  5); di += 5; }
        else                  { esc_desc[di++] = *src; }
    }
    esc_desc[di] = '\0';

    snprintf(out, cap,
        "<!doctype html>\n"
        "<html lang=\"en\"><head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
        "<title>ESP32-CAM provisioning</title>\n"
        "<style>\n"
        "  body{font-family:system-ui,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;color:#222}\n"
        "  h1{font-size:18px;margin:0 0 4px}\n"
        "  .mac{font-family:ui-monospace,monospace;color:#666;font-size:13px;margin-bottom:16px}\n"
        "  label{display:block;font-size:13px;margin:14px 0 4px}\n"
        "  input{width:100%%;padding:8px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;font-size:14px}\n"
        "  .hint{font-size:12px;color:#888;margin-top:2px}\n"
        "  button{margin-top:20px;width:100%%;padding:10px;background:#0066cc;color:#fff;border:0;border-radius:4px;font-size:15px;cursor:pointer}\n"
        "  button:disabled{background:#999;cursor:not-allowed}\n"
        "  .ok{color:#0a7c2f;margin-top:16px;display:none}\n"
        "  .err{color:#b00020;margin-top:16px;display:none;font-size:13px;white-space:pre-wrap}\n"
        "</style></head><body>\n"
        "<h1>ESP32-CAM provisioning</h1>\n"
        "<div class=\"mac\">Device: %s</div>\n"
        "<form id=\"f\">\n"
        "  <label for=\"wifi_ssid\">Wi-Fi SSID</label>\n"
        "  <input id=\"wifi_ssid\" name=\"wifi_ssid\" required maxlength=\"32\" placeholder=\"home-2.4\">\n"
        "  <label for=\"wifi_password\">Wi-Fi password</label>\n"
        "  <input id=\"wifi_password\" name=\"wifi_password\" type=\"password\" maxlength=\"63\" placeholder=\"\">\n"
        "  <div class=\"hint\">Leave blank for open networks.</div>\n"
        "  <label for=\"name\">Name <span class=\"hint\">(advisory; leave blank to keep current)</span></label>\n"
        "  <input id=\"name\" name=\"name\" maxlength=\"32\" value=\"%s\">\n"
        "  <label for=\"description\">Description <span class=\"hint\">(advisory; leave blank to keep current)</span></label>\n"
        "  <input id=\"description\" name=\"description\" maxlength=\"128\" value=\"%s\">\n"
        "  <button type=\"submit\">Configure &amp; reboot</button>\n"
        "</form>\n"
        "<div class=\"ok\" id=\"ok\">Configured. The device is rebooting \u2014 reconnect to your Wi-Fi network in a few seconds.</div>\n"
        "<div class=\"err\" id=\"err\"></div>\n"
        "<script>\n"
        "document.getElementById('f').addEventListener('submit',function(e){\n"
        "  e.preventDefault();\n"
        "  var b=document.querySelector('button');b.disabled=true;b.textContent='Sending...';\n"
        "  var body=JSON.stringify({\n"
        "    wifi_ssid:document.getElementById('wifi_ssid').value,\n"
        "    wifi_password:document.getElementById('wifi_password').value,\n"
        "    name:document.getElementById('name').value,\n"
        "    description:document.getElementById('description').value\n"
        "  });\n"
        "  fetch('/provision',{method:'POST',headers:{'Content-Type':'application/json'},body:body})\n"
        "    .then(function(r){if(r.ok){document.getElementById('ok').style.display='block';}\n"
        "      else{return r.text().then(function(t){throw new Error(t);});}})\n"
        "    .catch(function(e){var el=document.getElementById('err');el.textContent='Failed: '+e.message;el.style.display='block';\n"
        "      b.disabled=false;b.textContent='Configure &amp; reboot';});\n"
        "  return false;\n"
        "});\n"
        "</script>\n"
        "</body></html>\n",
        mac_hex12, esc_name, esc_desc);
}

esp_err_t home_get_handler_impl(httpd_req_t *req)
{
    /* The httpd worker task's stack is 4096 bytes (hardcoded in
     * IDF v5.5.3, NOT Kconfig-tunable — engram #3642). The handler
     * must keep its stack footprint minimal. We do this by:
     *   - MAC hex (13 bytes) and small locals on the stack
     *   - The rendered page buffer (~2.4 KB) on the HEAP, with
     *     MALLOC_CAP_SPIRAM so we don't fragment internal RAM
     *   - Freeing before return
     * The previous char page[4096] on the stack overflowed
     * immediately (engram #3641). */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char mac_hex[13];
    identity_mac_to_hex_lower(mac, mac_hex, sizeof(mac_hex));

    const config_t *cfg = (const config_t *)req->user_ctx;
    const char *name = (cfg && cfg->identity.name[0]) ? cfg->identity.name : "";
    const char *desc = (cfg && cfg->identity.description[0]) ? cfg->identity.description : "";

    /* Heap buffer (PSRAM preferred, internal RAM fallback). 3 KB
     * covers the full template (~2.1 KB) + max-length identity
     * strings (32 + 128 chars) with headroom. */
    const size_t page_cap = 3072;
    char *page = (char *)heap_caps_malloc(page_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!page) {
        /* PSRAM unavailable (shouldn't happen on AI-Thinker but
         * fall back to internal RAM if it does). */
        page = (char *)malloc(page_cap);
        if (!page) {
            /* Out of memory — send a minimal 500 inline rather than
             * calling httpd_resp_send_500 (which would need a stack
             * buffer too). The string literal lives in flash so it
             * doesn't consume RAM. */
            ESP_LOGE(TAG, "home page: out of memory (%u bytes)", (unsigned)page_cap);
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "500 Internal Server Error", 25);
            return ESP_FAIL;
        }
    }

    render_home_page(page, page_cap, mac_hex, name, desc);
    size_t page_len = strlen(page);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, page, (ssize_t)page_len);
    ESP_LOGI(TAG, "served home page (%u bytes) for mac=%s", (unsigned)page_len, mac_hex);
    free(page);
    return r;
}
