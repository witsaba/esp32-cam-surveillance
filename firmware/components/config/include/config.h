/* config.h — public API for the `config` module.
 *
 * Persists wifi credentials + identity Name/Description + a schema
 * version in NVS namespace `config` via per-key storage. Used by the
 * boot orchestrator (FW-03), provisioning (FW-05), and runtime config
 * commands (FW-18).
 *
 * Design notes (per docs/firmware-prd.md § FR-1a + § Module APIs):
 *
 *   - MAC is read live from eFuse by the identity module (FW-13.3);
 *     it is NOT stored in `config_t`. See PRD § FR-1a.
 *
 *   - Field caps: name ≤ 32 chars, description ≤ 128 chars (PRD
 *     § FR-1a). SSID and password caps are 32 / 63 respectively
 *     (32 = IEEE 802.11 SSID convention; 63 = WPA2 PMK length cap).
 *
 *   - The schema version is a `uint8_t` NVS key, compared against
 *     `CONFIG_SCHEMA_VERSION` (compiled-in). Stale entries
 *     (`stored != compiled`) fall back to defaults and set the dirty
 *     flag — see FW-02.2 / FW-02.3.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_WIFI_SSID_MAX         32u
#define CONFIG_WIFI_PASSWORD_MAX     63u
#define CONFIG_IDENTITY_NAME_MAX     32u
#define CONFIG_IDENTITY_DESC_MAX    128u

typedef struct {
    char ssid    [CONFIG_WIFI_SSID_MAX     + 1u];
    char password[CONFIG_WIFI_PASSWORD_MAX + 1u];
} wifi_creds_t;

typedef struct {
    char name       [CONFIG_IDENTITY_NAME_MAX + 1u];
    char description[CONFIG_IDENTITY_DESC_MAX + 1u];
} identity_t;

typedef struct {
    wifi_creds_t wifi;
    identity_t   identity;
    uint8_t      schema_version;  /* mirror of stored key; runtime-only */
    bool         dirty;           /* runtime-only; never persisted */
} config_t;

typedef enum {
    CONFIG_OK                  =  0,
    CONFIG_ERR_INVALID_ARG     = -1,
    CONFIG_ERR_NVS_INIT        = -2,
    CONFIG_ERR_NVS_OPEN        = -3,
    CONFIG_ERR_NVS_GET         = -4,
    CONFIG_ERR_NVS_SET         = -5,
    CONFIG_ERR_NVS_COMMIT      = -6,
    CONFIG_ERR_NVS_ERASE       = -7,
} config_status_t;

/* Compiled-in schema version. Bumping is a deliberate code change —
 * see design § "Compiled-in Schema Version" (no Kconfig symbol). */
#ifndef CONFIG_SCHEMA_VERSION
#define CONFIG_SCHEMA_VERSION 1u
#endif

config_status_t config_load(config_t *out, bool *out_dirty);
config_status_t config_save(const config_t *in);
config_status_t config_factory_reset(void);

#ifdef __cplusplus
}
#endif