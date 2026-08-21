/* mock_nvs_flash.h — In-memory NVS stub for host-side Unity tests.
 *
 * FW-02 test infrastructure. The `config` module calls `nvs_open`,
 * `nvs_set_str`, `nvs_get_str`, `nvs_set_u8`, `nvs_get_u8`,
 * `nvs_commit`, `nvs_erase_key`, `nvs_erase_all`, and `nvs_close`
 * via the macro overrides in `mock_nvs_flash_link.h`; those macros
 * redirect the calls to the `mock_nvs_*` functions defined here.
 *
 * On the host (idempotent for `idf.py test --target esp32`), the
 * mock stores entries in a process-global std::map keyed by
 * (namespace, key). On device builds (`MOCK_NVS_USE_REAL` is defined
 * automatically by the IDF NVS component), the macros are NOT
 * activated — see `mock_nvs_flash_link.h` — so the real NVS driver
 * is linked.
 *
 * Each test begins with `mock_nvs_reset()` to clear state.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reset all mock state (drop every namespace). Call from each
 * test's setup hook (before RUN_TEST). */
void mock_nvs_reset(void);

/* Pre-populate a string key in a given namespace. Used by tests that
 * simulate a previously-saved config (e.g. the stale-schema
 * FW-02.2 scenarios). */
void mock_nvs_seed_str(const char *namespace_name,
                       const char *key,
                       const char *value);

/* Pre-populate a uint8_t key. */
void mock_nvs_seed_u8(const char *namespace_name,
                      const char *key,
                      uint8_t value);

/* Read back a uint8_t key from the mock. Returns ESP_OK + writes
 * *out_value if present; ESP_ERR_NVS_NOT_FOUND otherwise. */
esp_err_t mock_nvs_read_u8(const char *namespace_name,
                           const char *key,
                           uint8_t *out_value);

/* Mock implementations of the NVS API. These are the targets of the
 * `#define nvs_open mock_nvs_open` macros in
 * `mock_nvs_flash_link.h`. Their signatures mirror the real NVS API
 * exactly so the redirect is transparent at call sites. */
esp_err_t mock_nvs_open(const char *namespace_name,
                        nvs_open_mode_t open_mode,
                        nvs_handle_t *out_handle);
void mock_nvs_close(nvs_handle_t handle);
esp_err_t mock_nvs_set_str(nvs_handle_t handle,
                           const char *key,
                           const char *value);
esp_err_t mock_nvs_get_str(nvs_handle_t handle,
                           const char *key,
                           char *out_value,
                           size_t *length);
esp_err_t mock_nvs_set_u8(nvs_handle_t handle,
                          const char *key,
                          uint8_t value);
esp_err_t mock_nvs_get_u8(nvs_handle_t handle,
                          const char *key,
                          uint8_t *out_value);
esp_err_t mock_nvs_commit(nvs_handle_t handle);
esp_err_t mock_nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t mock_nvs_erase_all(nvs_handle_t handle);

/* nvs_flash_init() / nvs_flash_erase() / nvs_get_stats() mocks so
 * that `app_main` and the device-side smoke check (commit 5) build
 * without real flash on host. */
esp_err_t mock_nvs_flash_init(void);
esp_err_t mock_nvs_flash_erase(void);
esp_err_t mock_nvs_get_stats(const char *partition_name,
                             nvs_stats_t *out_stats);

#ifdef __cplusplus
}
#endif