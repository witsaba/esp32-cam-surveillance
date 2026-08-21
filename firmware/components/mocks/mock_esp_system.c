/* mock_esp_system.c — implementation of the esp_system mocks. */
#include "mock_esp_system.h"

#include <string.h>

static uint8_t g_mac[6]           = {0,0,0,0,0,0};
static uint8_t g_mac_last[6]      = {0,0,0,0,0,0};
static int     g_mac_count        = 0;

static uint32_t g_chip_model      = CHIP_ESP32;
static uint32_t g_chip_revision   = 3;
static int      g_chip_count      = 0;

static const char *g_version_str  = "v5.5.3";
static const char *g_version_last = "";
static int         g_version_count = 0;

static int g_restart_count = 0;

void mock_esp_read_mac_set_bytes(const uint8_t mac[6])
{
    if (mac) memcpy(g_mac, mac, sizeof(g_mac));
}

void mock_esp_chip_info_set(uint32_t model, uint32_t revision)
{
    g_chip_model    = model;
    g_chip_revision = revision;
}

void mock_esp_get_idf_version_set(const char *ver)
{
    if (ver) g_version_str = ver;
}

int  mock_esp_read_mac_call_count(void)          { return g_mac_count; }
int  mock_esp_chip_info_call_count(void)         { return g_chip_count; }
int  mock_esp_get_idf_version_call_count(void)  { return g_version_count; }
int  mock_esp_restart_call_count(void)           { return g_restart_count; }

const uint8_t *mock_esp_read_mac_last_bytes(void)
{
    return g_mac_last;
}

const char *mock_esp_get_idf_version_last_returned(void)
{
    return g_version_last;
}

void mock_esp_system_reset(void)
{
    memset(g_mac, 0, sizeof(g_mac));
    memset(g_mac_last, 0, sizeof(g_mac_last));
    g_mac_count     = 0;
    g_chip_model    = CHIP_ESP32;
    g_chip_revision = 3;
    g_chip_count    = 0;
    g_version_str   = "v5.5.3";
    g_version_last  = "";
    g_version_count = 0;
    g_restart_count = 0;
}

esp_err_t mock_esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    (void)type;
    g_mac_count++;
    if (mac) {
        memcpy(mac, g_mac, sizeof(g_mac));
        memcpy(g_mac_last, g_mac, sizeof(g_mac_last));
    }
    return ESP_OK;
}

void mock_esp_chip_info(esp_chip_info_t *info)
{
    g_chip_count++;
    if (info) {
        info->model    = g_chip_model;
        info->revision = g_chip_revision;
        info->cores    = 2;
        info->features = 0;
    }
}

const char *mock_esp_get_idf_version(void)
{
    g_version_count++;
    g_version_last = g_version_str;
    return g_version_str;
}

void mock_esp_restart(void)
{
    g_restart_count++;
    /* Counter-only no-op. On host the test asserts call_count == 1. */
}
