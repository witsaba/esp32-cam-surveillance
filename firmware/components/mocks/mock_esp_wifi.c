/* mock_esp_wifi.c — implementation of the Wi-Fi driver mocks. */
#include "mock_esp_wifi.h"

#include <string.h>

static esp_err_t g_init_return     = ESP_OK;
static esp_err_t g_set_mode_return   = ESP_OK;
static esp_err_t g_set_config_return = ESP_OK;
static esp_err_t g_ap_start_return   = ESP_OK;
static esp_err_t g_stop_return       = ESP_OK;

static int g_init_count        = 0;
static int g_set_mode_count    = 0;
static int g_set_config_count  = 0;
static int g_ap_start_count    = 0;
static int g_stop_count        = 0;

static wifi_config_t g_last_config;
static int           g_last_iface = -1;
static int           g_captured   = 0;

void mock_esp_wifi_init_return_set(esp_err_t r)     { g_init_return     = r; }
void mock_esp_wifi_set_mode_return_set(esp_err_t r)   { g_set_mode_return   = r; }
void mock_esp_wifi_set_config_return_set(esp_err_t r) { g_set_config_return = r; }
void mock_esp_wifi_start_return_set(esp_err_t r)   { g_ap_start_return   = r; }
void mock_esp_wifi_stop_return_set(esp_err_t r)       { g_stop_return       = r; }

int  mock_esp_wifi_init_call_count(void)        { return g_init_count; }
int  mock_esp_wifi_set_mode_call_count(void)    { return g_set_mode_count; }
int  mock_esp_wifi_set_config_call_count(void)  { return g_set_config_count; }
int  mock_esp_wifi_start_call_count(void)    { return g_ap_start_count; }
int  mock_esp_wifi_stop_call_count(void)        { return g_stop_count; }

void mock_esp_wifi_set_config_capture(wifi_config_t *out)
{
    if (out) {
        memcpy(out, &g_last_config, sizeof(g_last_config));
    }
}

int mock_esp_wifi_set_config_capture_get_mode(wifi_interface_t *out)
{
    if (out) *out = g_last_iface;
    return g_captured;
}

void mock_esp_wifi_reset(void)
{
    g_init_return       = ESP_OK;
    g_set_mode_return   = ESP_OK;
    g_set_config_return = ESP_OK;
    g_ap_start_return   = ESP_OK;
    g_stop_return       = ESP_OK;
    g_init_count        = 0;
    g_set_mode_count    = 0;
    g_set_config_count  = 0;
    g_ap_start_count    = 0;
    g_stop_count        = 0;
    memset(&g_last_config, 0, sizeof(g_last_config));
    g_last_iface = -1;
    g_captured   = 0;
}

esp_err_t mock_esp_wifi_init(const wifi_init_config_t *cfg)
{
    (void)cfg;
    g_init_count++;
    return g_init_return;
}

esp_err_t mock_esp_wifi_set_mode(wifi_mode_t mode)
{
    (void)mode;
    g_set_mode_count++;
    return g_set_mode_return;
}

esp_err_t mock_esp_wifi_set_config(wifi_interface_t iface, wifi_config_t *cfg)
{
    g_set_config_count++;
    if (cfg) {
        g_last_config = *cfg;
        g_last_iface  = (int)iface;
        g_captured    = 1;
    }
    return g_set_config_return;
}

esp_err_t mock_esp_wifi_start(void)
{
    g_ap_start_count++;
    return g_ap_start_return;
}

esp_err_t mock_esp_wifi_stop(void)
{
    g_stop_count++;
    return g_stop_return;
}
