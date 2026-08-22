/* mock_esp_wifi.c — implementation of the Wi-Fi driver mocks. */
#include "mock_esp_wifi.h"

#include <string.h>

static esp_err_t g_init_return     = ESP_OK;
static esp_err_t g_set_mode_return   = ESP_OK;
static esp_err_t g_set_config_return = ESP_OK;
static esp_err_t g_ap_start_return   = ESP_OK;
static esp_err_t g_stop_return       = ESP_OK;
/* FW-08 — connect/disconnect return primes. */
static esp_err_t g_connect_return    = ESP_OK;
static esp_err_t g_disconnect_return = ESP_OK;

static int g_init_count        = 0;
static int g_set_mode_count    = 0;
static int g_set_config_count  = 0;
static int g_ap_start_count    = 0;
static int g_stop_count        = 0;
/* FW-08 — connect/disconnect counters. */
static int g_connect_count     = 0;
static int g_disconnect_count  = 0;

/* FW-08 — ring buffer of set_mode arguments. Capacity 32 covers
 * the worst-case boot sequence (1 boot-mode-set + N retry-mode-
 * flush sets). Out-of-range queries return (wifi_mode_t)0. */
#define WIFI_MODE_RING_CAP 32
static wifi_mode_t g_set_mode_args[WIFI_MODE_RING_CAP];
static size_t      g_set_mode_arg_head = 0;  /* next write slot */

static wifi_config_t g_last_config;
static int           g_last_iface = -1;
static int           g_captured   = 0;

void mock_esp_wifi_init_return_set(esp_err_t r)     { g_init_return     = r; }
void mock_esp_wifi_set_mode_return_set(esp_err_t r)   { g_set_mode_return   = r; }
void mock_esp_wifi_set_config_return_set(esp_err_t r) { g_set_config_return = r; }
void mock_esp_wifi_start_return_set(esp_err_t r)   { g_ap_start_return   = r; }
void mock_esp_wifi_stop_return_set(esp_err_t r)       { g_stop_return       = r; }
void mock_esp_wifi_connect_return_set(esp_err_t r)    { g_connect_return    = r; }
void mock_esp_wifi_disconnect_return_set(esp_err_t r) { g_disconnect_return = r; }

int  mock_esp_wifi_init_call_count(void)        { return g_init_count; }
int  mock_esp_wifi_set_mode_call_count(void)    { return g_set_mode_count; }
int  mock_esp_wifi_set_config_call_count(void)  { return g_set_config_count; }
int  mock_esp_wifi_start_call_count(void)    { return g_ap_start_count; }
int  mock_esp_wifi_stop_call_count(void)        { return g_stop_count; }
int  mock_esp_wifi_connect_call_count(void)     { return g_connect_count; }
int  mock_esp_wifi_disconnect_call_count(void)  { return g_disconnect_count; }

wifi_mode_t mock_esp_wifi_set_mode_arg_at(size_t idx)
{
    if (idx >= g_set_mode_arg_head) return (wifi_mode_t)0;
    /* newest-first: idx=0 returns the most recent argument. */
    return g_set_mode_args[g_set_mode_arg_head - 1 - idx];
}

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
    g_connect_return    = ESP_OK;
    g_disconnect_return = ESP_OK;
    g_init_count        = 0;
    g_set_mode_count    = 0;
    g_set_config_count  = 0;
    g_ap_start_count    = 0;
    g_stop_count        = 0;
    g_connect_count     = 0;
    g_disconnect_count  = 0;
    memset(g_set_mode_args, 0, sizeof(g_set_mode_args));
    g_set_mode_arg_head = 0;
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
    /* Record the argument in the ring buffer (newest-first queries
     * read from the head). Saturate at cap; oldest entries are
     * silently overwritten on overflow. */
    if (g_set_mode_arg_head < WIFI_MODE_RING_CAP) {
        g_set_mode_args[g_set_mode_arg_head++] = mode;
    } else {
        /* Shift left by one to keep the cap, losing the oldest. */
        memmove(g_set_mode_args, g_set_mode_args + 1,
                (WIFI_MODE_RING_CAP - 1) * sizeof(wifi_mode_t));
        g_set_mode_args[WIFI_MODE_RING_CAP - 1] = mode;
    }
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

esp_err_t mock_esp_wifi_connect(void)
{
    g_connect_count++;
    return g_connect_return;
}

esp_err_t mock_esp_wifi_disconnect(void)
{
    g_disconnect_count++;
    return g_disconnect_return;
}
