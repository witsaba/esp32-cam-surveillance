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

/* wifi_init_config_t — opaque stub for host. IDF's real struct has
 * ~30 fields; on host the mock ignores them. We declare a typedef so
 * the production source compiles unchanged on host. The macro
 * WIFI_INIT_CONFIG_DEFAULT() zero-initializes the struct; IDF's real
 * macro sets ~30 default values, but the host mock never reads them. */
typedef struct {
    int _placeholder;
} wifi_init_config_t;

#ifndef WIFI_INIT_CONFIG_DEFAULT
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){ ._placeholder = 0 })
#endif

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
        uint8_t ssid_hidden;
        uint8_t max_connection;
        uint16_t beacon_interval;
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

/* WIFI_AP_DEFAULT_CONFIG() — IDF macro returns a wifi_config_t with
 * the .ap field populated with sensible defaults. On host we provide
 * a minimal subset that matches the device side:
 *   max_connection = 4 (load-bearing — caught on device interaction,
 *     engram #3636)
 *   authmode = 0 (WIFI_AUTH_OPEN)
 *   channel = 1, beacon_interval = 100
 * The mock's esp_wifi_set_config discards the struct contents, so
 * the other defaults are placeholders. */
#ifndef WIFI_AP_DEFAULT_CONFIG
#define WIFI_AP_DEFAULT_CONFIG() ((wifi_config_t){ \
    .ap = { \
        .ssid = {0}, \
        .password = {0}, \
        .ssid_len = 0, \
        .channel = 1, \
        .authmode = 0, \
        .max_connection = 4, \
    }, \
})
#endif

/* FW-08 — WIFI_MODE_APSTA / WIFI_MODE_STA constants matching
 * IDF v5.5.3 (esp_wifi_types_generic.h:20-32). The wifi component
 * uses WIFI_MODE_APSTA when softap_is_active() is true at init
 * time (FW-08.5) and WIFI_MODE_STA otherwise (FW-08.6 + the
 * post-IP-up teardown). */
#ifndef WIFI_MODE_NULL
#define WIFI_MODE_NULL   0
#define WIFI_MODE_STA    1
#define WIFI_MODE_AP     2
#define WIFI_MODE_APSTA  3
#endif

/* WIFI_IF_STA matches IDF v5.5.3 (esp_wifi_types.h). */
#ifndef WIFI_IF_STA
#define WIFI_IF_STA      0
#endif

/* ---------- primable return values (test helpers) ---------- */
void mock_esp_wifi_init_return_set(esp_err_t r);
void mock_esp_wifi_set_mode_return_set(esp_err_t r);
void mock_esp_wifi_set_config_return_set(esp_err_t r);
void mock_esp_wifi_start_return_set(esp_err_t r);
void mock_esp_wifi_stop_return_set(esp_err_t r);
/* FW-08 — esp_wifi_connect / esp_wifi_disconnect return primes. */
void mock_esp_wifi_connect_return_set(esp_err_t r);
void mock_esp_wifi_disconnect_return_set(esp_err_t r);

/* ---------- call counters / captured state ---------- */
int  mock_esp_wifi_init_call_count(void);
int  mock_esp_wifi_set_mode_call_count(void);
int  mock_esp_wifi_set_config_call_count(void);
int  mock_esp_wifi_start_call_count(void);
int  mock_esp_wifi_stop_call_count(void);
/* FW-08 — connect/disconnect counters. */
int  mock_esp_wifi_connect_call_count(void);
int  mock_esp_wifi_disconnect_call_count(void);
/* FW-08 — wifi_mode_t argument of the idx-th esp_wifi_set_mode
 * call (0-indexed, newest-first via ring buffer of cap 32).
 * Out-of-range returns WIFI_MODE_NULL (0). */
wifi_mode_t mock_esp_wifi_set_mode_arg_at(size_t idx);
void mock_esp_wifi_set_config_capture(wifi_config_t *out);
int  mock_esp_wifi_set_config_capture_get_mode(wifi_interface_t *out);
/* FW-13 — esp_wifi_sta_get_rssi() primer + getter + counter.
 * The status-frame builder reads rssi_dbm from this getter. */
void mock_esp_wifi_set_rssi_dbm(int32_t rssi);
int  mock_esp_wifi_get_rssi_call_count(void);
int32_t mock_esp_wifi_get_rssi_dbm(void);
/* ---------- reset ---------- */
void mock_esp_wifi_reset(void);

/* ---------- mock targets (called via the link-header redirect) ---------- */
esp_err_t mock_esp_wifi_init(const wifi_init_config_t *cfg);
esp_err_t mock_esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t mock_esp_wifi_set_config(wifi_interface_t iface, wifi_config_t *cfg);
esp_err_t mock_esp_wifi_start(void);
esp_err_t mock_esp_wifi_stop(void);
/* FW-08 — station connect/disconnect. */
esp_err_t mock_esp_wifi_connect(void);
esp_err_t mock_esp_wifi_disconnect(void);
/* FW-13 — esp_wifi_sta_get_rssi() redirect target. The mock writes
 * the primed RSSI to *rssi and returns ESP_OK. */
esp_err_t mock_esp_wifi_sta_get_rssi(int32_t *rssi);
