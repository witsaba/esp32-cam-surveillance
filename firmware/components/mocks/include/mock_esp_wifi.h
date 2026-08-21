/* mock_esp_wifi.h — host-side mock for the ESP-IDF Wi-Fi driver.
 *
 * FW-05 softAP provisioning exercises the IDF Wi-Fi API:
 *   esp_wifi_set_mode(WIFI_MODE_AP)
 *   esp_wifi_set_config(WIFI_IF_AP, &cfg)
 *   esp_wifi_start()
 *   esp_wifi_stop()             (used by softap_stop; FW-08 future)
 *
 * On host, the production source includes `mock_esp_wifi_link.h`
 * which `#define`s each production symbol to the mock symbol below.
 * The mock returns whatever the test primed via `set_*_return_set()`
 * (default ESP_OK) and records call counts for assertions.
 *
 * On device builds, `MOCK_WIFI_USE_REAL` is defined (or the link
 * header is not included at all) and the real esp_wifi_* functions
 * from IDF are linked.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Forward-declared types matching IDF's esp_wifi_types.h. On host
 * we don't pull in IDF; tests don't construct these types directly.
 * The mock_esp_wifi_set_config_capture() writes whatever the
 * production source passed, but only inspects a small subset. */
typedef int wifi_mode_t;
typedef int wifi_interface_t;
typedef struct {
    /* The capture inspects the SSID slot (wifi_config_t.ap.ssid[32])
     * for the FW-05.1 whoami S3 / bring-up assertions. Other slots
     * are opaque. */
    struct {
        uint8_t ssid[32];
        uint8_t password[64];
        uint8_t ssid_len;
        uint8_t channel;
        uint8_t authmode;
        uint8_t max_connection;
    } ap;
    struct {
        uint8_t ssid[32];
        uint8_t password[64];
        uint8_t ssid_len;
        uint8_t channel;
        uint8_t scan_method;
        uint8_t sort_method;
        uint8_t threshold_rssi;
        uint8_t threshold_authmode;
        uint8_t pmf_mode;
        uint8_t rm_enabled;
        uint8_t btm_enabled;
        uint8_t mbo_enabled;
        uint8_t reserved[8];
    } sta;
} wifi_config_t;

/* ---------- primable return values (test helpers) ---------- */
void mock_esp_wifi_set_mode_return_set(esp_err_t r);
void mock_esp_wifi_set_config_return_set(esp_err_t r);
void mock_esp_wifi_start_return_set(esp_err_t r);
void mock_esp_wifi_stop_return_set(esp_err_t r);

/* ---------- call counters / captured state ---------- */
int  mock_esp_wifi_set_mode_call_count(void);
int  mock_esp_wifi_set_config_call_count(void);
int  mock_esp_wifi_start_call_count(void);
int  mock_esp_wifi_stop_call_count(void);
void mock_esp_wifi_set_config_capture(wifi_config_t *out);
int  mock_esp_wifi_set_config_capture_get_mode(wifi_interface_t *out);

/* ---------- reset ---------- */
void mock_esp_wifi_reset(void);

/* ---------- mock targets (called via the link-header redirect) ---------- */
esp_err_t mock_esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t mock_esp_wifi_set_config(wifi_interface_t iface, wifi_config_t *cfg);
esp_err_t mock_esp_wifi_start(void);
esp_err_t mock_esp_wifi_stop(void);
