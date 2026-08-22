/* mock_esp_netif.c — implementation of the netif mocks. */
#include "mock_esp_netif.h"

/* Sentinel handle — non-NULL, never dereferenced. The mock only
 * stores/counts the value; the production source passes it back to
 * esp_netif_destroy() which the mock discards. */
static char g_sentinel_ap;
static esp_netif_handle_t g_create_return = &g_sentinel_ap;
static esp_err_t          g_init_return    = ESP_OK;
static esp_err_t          g_destroy_return = ESP_OK;

static int g_init_count    = 0;
static int g_create_count  = 0;
static int g_destroy_count = 0;

void mock_esp_netif_init_return_set(esp_err_t r)
{
    g_init_return = r;
}

void mock_esp_netif_create_default_wifi_ap_return_set(esp_netif_handle_t h)
{
    g_create_return = h;
}

void mock_esp_netif_destroy_return_set(esp_err_t r)
{
    g_destroy_return = r;
}

int mock_esp_netif_init_call_count(void)    { return g_init_count; }
int mock_esp_netif_create_default_wifi_ap_call_count(void) { return g_create_count; }
int mock_esp_netif_destroy_call_count(void) { return g_destroy_count; }

void mock_esp_netif_reset(void)
{
    g_init_return    = ESP_OK;
    g_create_return = &g_sentinel_ap;
    g_destroy_return = ESP_OK;
    g_init_count     = 0;
    g_create_count   = 0;
    g_destroy_count  = 0;
}

esp_err_t mock_esp_netif_init(void)
{
    g_init_count++;
    return g_init_return;
}

esp_netif_handle_t mock_esp_netif_create_default_wifi_ap(void)
{
    g_create_count++;
    return g_create_return;
}

esp_err_t mock_esp_netif_destroy(esp_netif_t *netif)
{
    (void)netif;
    g_destroy_count++;
    return g_destroy_return;
}
