/* mock_gpio.c — implementation of the GPIO driver mocks. */
#include "mock_gpio.h"

#include <string.h>

static esp_err_t g_config_return     = ESP_OK;
static esp_err_t g_set_level_return   = ESP_OK;

static int g_config_count  = 0;
static int g_set_level_count = 0;

static int g_last_pin    = -1;
static int g_last_level  = -1;
static int g_captured    = 0;

void mock_gpio_config_set_return(esp_err_t r)       { g_config_return   = r; }
void mock_gpio_set_level_set_return(esp_err_t r)    { g_set_level_return = r; }

int mock_gpio_config_call_count(void)        { return g_config_count; }
int mock_gpio_set_level_call_count(void)     { return g_set_level_count; }

void mock_gpio_set_level_capture(int *out_pin, int *out_level)
{
    if (out_pin)   *out_pin   = g_last_pin;
    if (out_level) *out_level = g_last_level;
}

void mock_gpio_reset(void)
{
    g_config_return    = ESP_OK;
    g_set_level_return = ESP_OK;
    g_config_count     = 0;
    g_set_level_count  = 0;
    g_last_pin         = -1;
    g_last_level       = -1;
    g_captured         = 0;
}

esp_err_t mock_gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    g_config_count++;
    return g_config_return;
}

esp_err_t mock_gpio_set_level(gpio_num_t pin, uint32_t level)
{
    g_set_level_count++;
    g_last_pin   = (int)pin;
    g_last_level = (int)(level ? 1 : 0);
    g_captured   = 1;
    return g_set_level_return;
}
