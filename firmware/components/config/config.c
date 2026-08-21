/* config.c — implementation of the `config` module.
 *
 * Persists `config_t` (wifi credentials + identity Name/Description +
 * schema version) in NVS namespace `config` via per-key writes. The
 * host test runner stubs every `nvs_*` call to the in-memory mock via
 * the `mock_nvs_flash_link.h` macro overrides; device builds link
 * against the real NVS driver (the macros are gated by
 * `MOCK_NVS_USE_REAL`).
 *
 * The schema-version guard (FW-02.2 + FW-02.3) is implemented behind
 * the `CONFIG_TEST_STUB_VERSION_CHECK` macro so the bite-proof
 * scenario can stub it. When the macro is undefined (default), the
 * version check is real and `stored != CONFIG_SCHEMA_VERSION` triggers
 * the defaults + dirty path.
 */
#include "config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#ifdef UNITY_HOST_BUILD
/* Host test build: redirect every nvs_* call to the in-memory mock. */
#include "mock_nvs_flash_link.h"
#endif

static const char *TAG = "config";

/* On host, nvs_* calls are redirected via the mock_nvs_flash_link.h
 * macros (included above); on device, the real nvs_flash driver is
 * linked and the macros do nothing because MOCK_NVS_USE_REAL is not
 * defined. The mock-redirect macros are also gated by `#ifndef
 * MOCK_NVS_USE_REAL`, so they are safe to include even if the real
 * driver is in use — but we omit the include on device to keep the
 * production build free of any mock dependency. */

/* Internal helpers — extracted from the load/save loop for testability
 * and clarity (Strict TDD refactor step in commit 2). */
static config_status_t write_str_field(nvs_handle_t h, const char *key, const char *value);

static config_status_t write_str_field(nvs_handle_t h, const char *key, const char *value)
{
    if (!value) {
        return CONFIG_ERR_INVALID_ARG;
    }
    /* Empty strings are erased (not stored as "") to keep the
     * namespace minimal — see design § Internal Flows / config_save. */
    if (value[0] == '\0') {
        esp_err_t err = nvs_erase_key(h, key);
        return (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND)
            ? CONFIG_OK : CONFIG_ERR_NVS_SET;
    }
    esp_err_t err = nvs_set_str(h, key, value);
    return (err == ESP_OK) ? CONFIG_OK : CONFIG_ERR_NVS_SET;
}

/* Internal: returns true iff the stored schema version is stale.
 * Wrapped in #ifndef CONFIG_TEST_STUB_VERSION_CHECK so the FW-02.3
 * bite-proof test can stub it via -DCONFIG_TEST_STUB_VERSION_CHECK.
 */
static bool config_schema_is_stale(uint8_t stored)
{
#ifndef CONFIG_TEST_STUB_VERSION_CHECK
    return stored != CONFIG_SCHEMA_VERSION;
#else
    /* Stubbed: always "fresh" so the version check is bypassed.
     * The FW-02.3 bite-proof test must fail when this stub is in
     * effect. */
    (void)stored;
    return false;
#endif
}

config_status_t config_load(config_t *out, bool *out_dirty)
{
    if (!out || !out_dirty) {
        return CONFIG_ERR_INVALID_ARG;
    }
    *out_dirty = false;
    memset(out, 0, sizeof(*out));
    out->schema_version = CONFIG_SCHEMA_VERSION;

    nvs_handle_t h;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Fresh partition — not an error. Defaults are valid; mark
         * dirty so the boot orchestrator (FW-03) knows to persist. */
        *out_dirty = true;
        return CONFIG_OK;
    }
    if (err != ESP_OK) {
        return CONFIG_ERR_NVS_OPEN;
    }

    uint8_t stored = 0;
    err = nvs_get_u8(h, "schema_version", &stored);
    /* Missing schema_version key == stored == 0, which is stale. */
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return CONFIG_ERR_NVS_GET;
    }

    if (config_schema_is_stale(stored)) {
        ESP_LOGW(TAG,
                 "schema_version mismatch: stored=%u compiled=%u — "
                 "restoring defaults",
                 (unsigned)stored, (unsigned)CONFIG_SCHEMA_VERSION);
        nvs_close(h);
        memset(out, 0, sizeof(*out));
        out->schema_version = CONFIG_SCHEMA_VERSION;
        *out_dirty = true;
        return CONFIG_OK;
    }

    /* Schema matches — read each string key. Absence == empty
     * string (NOT an error). */
    size_t len;
    char *const fields[] = {
        out->wifi.ssid,
        out->wifi.password,
        out->identity.name,
        out->identity.description,
    };
    const char *const keys[] = {
        "ssid", "password", "name", "description",
    };
    const size_t caps[] = {
        sizeof(out->wifi.ssid),
        sizeof(out->wifi.password),
        sizeof(out->identity.name),
        sizeof(out->identity.description),
    };
    for (size_t i = 0; i < 4; ++i) {
        fields[i][0] = '\0';  /* ensure NUL-terminated on miss */
        len = caps[i];
        err = nvs_get_str(h, keys[i], fields[i], &len);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(h);
            return CONFIG_ERR_NVS_GET;
        }
    }

    nvs_close(h);
    return CONFIG_OK;
}

config_status_t config_save(const config_t *in)
{
    if (!in) {
        return CONFIG_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return CONFIG_ERR_NVS_OPEN;
    }

    config_status_t st;
    if ((st = write_str_field(h, "ssid",        in->wifi.ssid))        != CONFIG_OK ||
        (st = write_str_field(h, "password",    in->wifi.password))    != CONFIG_OK ||
        (st = write_str_field(h, "name",        in->identity.name))    != CONFIG_OK ||
        (st = write_str_field(h, "description", in->identity.description)) != CONFIG_OK) {
        nvs_close(h);
        return st;
    }

    err = nvs_set_u8(h, "schema_version", CONFIG_SCHEMA_VERSION);
    if (err != ESP_OK) {
        nvs_close(h);
        return CONFIG_ERR_NVS_SET;
    }

    err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK) ? CONFIG_OK : CONFIG_ERR_NVS_COMMIT;
}

config_status_t config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return CONFIG_OK;  /* already empty */
    }
    if (err != ESP_OK) {
        return CONFIG_ERR_NVS_OPEN;
    }

    err = nvs_erase_all(h);
    if (err != ESP_OK) {
        nvs_close(h);
        return CONFIG_ERR_NVS_ERASE;
    }
    err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK) ? CONFIG_OK : CONFIG_ERR_NVS_COMMIT;
}