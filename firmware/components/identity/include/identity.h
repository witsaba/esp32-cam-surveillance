/* identity.h — public API for the FW-13 identity shared module.
 *
 * The identity module unifies three pieces of canonical device
 * identity that previously lived in different places:
 *
 *   1. MAC address — read live from eFuse via esp_read_mac().
 *      Source of truth for hardware identity (FW-13.3 S1 +
 *      PRD § FR-1a "MAC = canonical identity, from eFuse, not NVS").
 *
 *   2. Name + Description — stored in NVS namespace `config`
 *      under keys `name` and `description`. Written by the
 *      FW-05 /provision handler; read here for use in the
 *      hello + status JSON payloads.
 *
 *   3. Lowercase-hex formatted MAC string — 12 chars + NUL,
 *      no separators (e.g. `c8f09e9d5008`). This is the
 *      canonical wire format for the hello MAC field (charter
 *      L1196 + PRD § FR-1a).
 *
 * The `identity_t` type is the FW-13 runtime carrier; it is
 * distinct from `config.h`'s `identity_t` (which only carries
 * name + description for the wifi-credentials config blob).
 * Use `device_identity_t` here to avoid the namespace collision
 * with `config.h` — the two types share the name + description
 * fields by design (FW-13.3's planned refactor will eventually
 * fold the NVS-read into config_load, but for now they live
 * side-by-side and the field caps differ:
 *   config.h  : CONFIG_IDENTITY_NAME_MAX=32, _DESC_MAX=128
 *   identity.h: CONFIG_FIRMWARE_IDENTITY_NAME_MAX_LEN=32,
 *               CONFIG_FIRMWARE_IDENTITY_DESCRIPTION_MAX_LEN=64
 *
 * The two caps differ because FW-13's hello/status payloads
 * are short on purpose (backend parsing surface); softAP home
 * page + /whoami keep the longer description cap.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Render a 6-byte MAC as 12-char lower-hex WITHOUT separators
 * (e.g. {0xc8,0xf0,0x9e,0x9d,0x50,0x08} -> "c8f09e9d5008").
 *
 * `out_len` must be >= 13 (12 hex chars + NUL terminator).
 *
 * Returns:
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_ARG if `mac` is NULL, `out` is NULL, or
 *     `out_len < 13`.
 *
 * On ESP_ERR_INVALID_ARG the buffer is left untouched. On
 * success the 12 hex chars + trailing NUL are written. */
esp_err_t identity_mac_to_hex_lower(const uint8_t mac[6],
                                     char *out,
                                     size_t out_len);

/* The FW-13 device-identity carrier struct.
 *
 * Fields:
 *   mac[6]            - raw bytes from esp_read_mac(...).
 *   mac_hex[13]       - 12-char lowercase hex + NUL (output of
 *                       identity_mac_to_hex_lower).
 *   name[]            - device name from NVS `config.name`; empty
 *                       string when NVS key is missing.
 *   description[]     - device description from NVS
 *                       `config.description`; empty string when
 *                       NVS key is missing.
 *
 * Sized by the two CONFIG_FIRMWARE_IDENTITY_*_MAX_LEN symbols
 * (Kconfig defaults: 32 name + 64 description, range 1..128
 * and 1..256 respectively). The Kconfig symbols are sourced
 * from firmware/main/Kconfig.projbuild (which sources this
 * component's Kconfig). */
typedef struct {
    uint8_t mac[6];
    char    mac_hex[13];
    char    name       [CONFIG_FIRMWARE_IDENTITY_NAME_MAX_LEN];
    char    description[CONFIG_FIRMWARE_IDENTITY_DESCRIPTION_MAX_LEN];
} device_identity_t;

/* Load identity from eFuse (MAC) + NVS config namespace
 * (name + description).
 *
 * `out` MUST be non-NULL. The function zero-initialises the
 * struct before writing, so partial reads (e.g. NVS missing)
 * still leave the struct in a known state.
 *
 * On NVS missing or empty, `out->name` and `out->description`
 * are set to "" and a warning is logged via ESP_LOGW. The MAC
 * is always populated (eFuse read is non-failing on host +
 * device). Returns ESP_OK on success.
 *
 * This function does NOT fail when NVS is missing — it logs +
 * falls back to empty strings (per design smell #2 resolution:
 * "skip + log" semantics, not hard-error). The caller can
 * detect the empty-state via `out->name[0] == '\0'`. */
esp_err_t identity_load(device_identity_t *out);

#ifdef __cplusplus
}
#endif