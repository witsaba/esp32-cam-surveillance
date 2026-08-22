/* mock_esp_system.h — host-side mock for the IDF system layer.
 *
 * Redirects:
 *   esp_read_mac(mac, type)
 *   esp_chip_info(info)
 *   esp_get_idf_version() -> const char *
 *   esp_restart()
 *
 * `esp_read_mac` is primed by `mock_esp_read_mac_set_bytes` (default
 * zeros). `esp_chip_info` is primed by `mock_esp_chip_info_set`.
 * `esp_get_idf_version` is primed by `mock_esp_get_idf_version_set`.
 * `esp_restart` is a counter-only no-op — production calls it once
 * after sending the 200 response on /provision.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Forward-declared to match IDF. The mock uses int (enums are int
 * on host) and a tiny opaque struct for esp_chip_info_t. */
typedef int esp_mac_type_t;
#define ESP_MAC_WIFI_STA 0
#define ESP_MAC_WIFI_AP  1
#define ESP_MAC_BT       2
#define ESP_MAC_ETH      3

typedef struct {
    uint32_t model;
    uint32_t revision;
    uint32_t cores;
    uint32_t features;
} esp_chip_info_t;

/* chip model constants (mirrors IDF soc_caps.h subset). */
#define CHIP_ESP32       1
#define CHIP_ESP32S2     2
#define CHIP_ESP32S3     5
#define CHIP_ESP32C3     12

/* ---------- primers ---------- */
void mock_esp_read_mac_set_bytes(const uint8_t mac[6]);
void mock_esp_chip_info_set(uint32_t model, uint32_t revision);
void mock_esp_get_idf_version_set(const char *ver);

/* ---------- call counters / state ---------- */
int  mock_esp_read_mac_call_count(void);
int  mock_esp_chip_info_call_count(void);
int  mock_esp_get_idf_version_call_count(void);
int  mock_esp_restart_call_count(void);
const uint8_t *mock_esp_read_mac_last_bytes(void);
const char   *mock_esp_get_idf_version_last_returned(void);

/* ---------- reset ---------- */
void mock_esp_system_reset(void);

/* ---------- mock targets ---------- */
esp_err_t mock_esp_read_mac(uint8_t *mac, esp_mac_type_t type);
void      mock_esp_chip_info(esp_chip_info_t *info);
const char *mock_esp_get_idf_version(void);
void      mock_esp_restart(void);
