/* mock_esp_netif.c — implementation of the netif mocks. */
#include "mock_esp_netif.h"

/* Sentinel handle — non-NULL, never dereferenced. The mock only
 * stores/counts the value; the production source passes it back to
 * esp_netif_destroy() which the mock discards. */
static char g_sentinel_ap;
/* FW-08 — separate sentinel for the STA netif so AP/STA handles
 * are distinguishable in test assertions. */
static char g_sentinel_sta;
static esp_netif_handle_t g_create_ap_return  = &g_sentinel_ap;
static esp_netif_handle_t g_create_sta_return = &g_sentinel_sta;
static esp_err_t          g_init_return       = ESP_OK;
static esp_err_t          g_set_default_return = ESP_OK;
static esp_err_t          g_destroy_return    = ESP_OK;

static int g_init_count        = 0;
static int g_create_ap_count   = 0;
static int g_create_sta_count  = 0;
static int g_set_default_count = 0;
static int g_destroy_count     = 0;

void mock_esp_netif_init_return_set(esp_err_t r)
{
    g_init_return = r;
}

void mock_esp_netif_create_default_wifi_ap_return_set(esp_netif_handle_t h)
{
    g_create_ap_return = h;
}

/* FW-08 — STA netif return prime. */
void mock_esp_netif_create_default_wifi_sta_return_set(esp_netif_handle_t h)
{
    g_create_sta_return = h;
}

void mock_esp_netif_set_default_netif_return_set(esp_err_t r)
{
    g_set_default_return = r;
}

void mock_esp_netif_destroy_return_set(esp_err_t r)
{
    g_destroy_return = r;
}

int mock_esp_netif_init_call_count(void)        { return g_init_count; }
int mock_esp_netif_create_default_wifi_ap_call_count(void) { return g_create_ap_count; }
int mock_esp_netif_create_default_wifi_sta_call_count(void) { return g_create_sta_count; }
int mock_esp_netif_set_default_netif_call_count(void) { return g_set_default_count; }
int mock_esp_netif_destroy_call_count(void)      { return g_destroy_count; }

void mock_esp_netif_reset(void)
{
    g_init_return        = ESP_OK;
    g_set_default_return = ESP_OK;
    g_create_ap_return   = &g_sentinel_ap;
    g_create_sta_return  = &g_sentinel_sta;
    g_destroy_return     = ESP_OK;
    g_init_count         = 0;
    g_set_default_count  = 0;
    g_create_ap_count    = 0;
    g_create_sta_count   = 0;
    g_destroy_count      = 0;
}

esp_err_t mock_esp_netif_init(void)
{
    g_init_count++;
    return g_init_return;
}

esp_netif_handle_t mock_esp_netif_create_default_wifi_ap(void)
{
    g_create_ap_count++;
    return g_create_ap_return;
}

/* FW-08 — STA netif mock target. */
esp_netif_handle_t mock_esp_netif_create_default_wifi_sta(void)
{
    g_create_sta_count++;
    return g_create_sta_return;
}

esp_err_t mock_esp_netif_set_default_netif(esp_netif_t *netif)
{
    (void)netif;
    g_set_default_count++;
    return g_set_default_return;
}

esp_err_t mock_esp_netif_destroy(esp_netif_t *netif)
{
    (void)netif;
    g_destroy_count++;
    return g_destroy_return;
}
