/* mock_esp_event.c — implementation of the event loop mocks. */
#include "mock_esp_event.h"

static esp_err_t g_create_return = ESP_OK;
static int       g_create_count  = 0;

void mock_esp_event_loop_create_default_return_set(esp_err_t r)
{
    g_create_return = r;
}

int mock_esp_event_loop_create_default_call_count(void)
{
    return g_create_count;
}

void mock_esp_event_reset(void)
{
    g_create_return = ESP_OK;
    g_create_count  = 0;
}

esp_err_t mock_esp_event_loop_create_default(void)
{
    g_create_count++;
    return g_create_return;
}
