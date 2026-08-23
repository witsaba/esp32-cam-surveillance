/* identity.c — implementation of the FW-13 identity shared module.
 *
 * Two entry points:
 *   - identity_mac_to_hex_lower(): pure hex-encode (testable in
 *     isolation; the 3 tests in test_identity_mac_hex.c exercise
 *     this surface directly).
 *   - identity_load(): reads eFuse MAC + NVS name/description
 *     into a device_identity_t. NVS missing/empty -> empty
 *     strings + ESP_LOGW warning (does NOT fail the call).
 *
 * Host/device divergence:
 *   On host (UNITY_HOST_BUILD), `esp_read_mac` and `nvs_get_str`
 *   are redirected to the in-memory mocks via the link-header
 *   pattern (mock_esp_system_link.h, mock_nvs_flash_link.h).
 *   On device, the real IDF symbols are linked.
 */
#include "identity.h"

#include <string.h>

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_system_link.h"
#include "mock_nvs_flash_link.h"
#else
#include "esp_system.h"
#include "nvs_flash.h>
#include "nvs.h"
#endif

/* esp_mac_type_t constants (mirror mock_esp_system.h). The host
 * build gets them from mock_esp_system.h; the device build gets
 * them from esp_mac.h. We declare them locally so identity.c
 * compiles on both without including either header (the link
 * header already pulls the right one in). */
#ifndef ESP_MAC_WIFI_STA
#define ESP_MAC_WIFI_STA 0
#endif

static const char *TAG = "identity";

esp_err_t identity_mac_to_hex_lower(const uint8_t mac[6],
                                     char *out,
                                     size_t out_len)
{
    if (!mac || !out || out_len < 13) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Lowercase hex alphabet — per charter L1196 + the softAP
     * `render_mac_lowercase` helper at softap_handlers.c:103
     * (both produce identical output; T-13-F will fold them
     * into this single canonical helper). */
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        out[i * 2]     = hex[(mac[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[mac[i] & 0x0F];
    }
    out[12] = '\0';
    return ESP_OK;
}

esp_err_t identity_load(device_identity_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    /* Zero-init first — guards against partial reads when NVS
     * keys are missing (the explicit design smell #2 resolution:
     * memset before any writes so the struct stays in a known
     * state). */
    memset(out, 0, sizeof(*out));

    /* Step 1: read MAC from eFuse. Per design smell #8 resolution
     * we use `esp_read_mac(..., ESP_MAC_WIFI_STA)` (matches the
     * softAP convention at softap_handlers.c:123 + the existing
     * mock surface in mock_esp_system.c). On host the mock
     * returns the bytes primed via mock_esp_read_mac_set_bytes(). */
    esp_err_t r = esp_read_mac(out->mac, ESP_MAC_WIFI_STA);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "identity_load: esp_read_mac failed: %s",
                 esp_err_to_name(r));
        return r;
    }

    /* Step 2: format MAC as 12-char lowercase hex + NUL. The
     * `out_len` precondition for mac_hex is `>= 13` — the struct
     * declares `mac_hex[13]` so this is structurally guaranteed. */
    r = identity_mac_to_hex_lower(out->mac, out->mac_hex, sizeof(out->mac_hex));
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "identity_load: identity_mac_to_hex_lower failed");
        return r;
    }

    /* Step 3: read NVS `config` namespace keys. We open a fresh
     * handle per call (the production call frequency is one-per-
     * hello-frame which is fine; NVS handles are cheap). */
    nvs_handle_t handle;
    r = nvs_open("config", NVS_READONLY, &handle);
    if (r != ESP_OK) {
        /* NVS namespace missing or unopenable (unprovisioned device,
         * factory-reset state). Per design smell #2 we DO NOT fail
         * the call — log + leave name/description as empty strings. */
        ESP_LOGW(TAG, "identity_load: nvs_open(config) failed: %s; "
                       "name/description will be empty",
                 esp_err_to_name(r));
        return ESP_OK;
    }

    /* name — read into the struct's fixed-size buffer. The Kconfig
     * cap is CONFIG_FIRMWARE_IDENTITY_NAME_MAX_LEN; nvs_get_str
     * truncates if the stored value exceeds the buffer (returns
     * ESP_OK + the truncated bytes). */
    size_t name_len = sizeof(out->name);
    r = nvs_get_str(handle, "name", out->name, &name_len);
    if (r == ESP_OK) {
        /* nvs_get_str writes a NUL terminator on success. */
    } else if (r == ESP_ERR_NVS_NOT_FOUND) {
        out->name[0] = '\0';
        ESP_LOGW(TAG, "identity_load: NVS config.name missing; "
                       "using empty string");
    } else {
        /* Other NVS error — log + fall back to empty string. */
        out->name[0] = '\0';
        ESP_LOGW(TAG, "identity_load: nvs_get_str(name) failed: %s; "
                       "using empty string",
                 esp_err_to_name(r));
    }

    /* description — same shape as name. */
    size_t desc_len = sizeof(out->description);
    r = nvs_get_str(handle, "description", out->description, &desc_len);
    if (r == ESP_OK) {
        /* nvs_get_str NUL-terminates on success. */
    } else if (r == ESP_ERR_NVS_NOT_FOUND) {
        out->description[0] = '\0';
        ESP_LOGW(TAG, "identity_load: NVS config.description missing; "
                       "using empty string");
    } else {
        out->description[0] = '\0';
        ESP_LOGW(TAG, "identity_load: nvs_get_str(description) failed: %s; "
                       "using empty string",
                 esp_err_to_name(r));
    }

    nvs_close(handle);
    return ESP_OK;
}