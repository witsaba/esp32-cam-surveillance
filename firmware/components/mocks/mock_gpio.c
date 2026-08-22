/* mock_gpio.c — implementation of the GPIO driver mocks. */
#include "mock_gpio.h"

#include <string.h>

static esp_err_t g_config_return     = ESP_OK;
static esp_err_t g_set_level_return   = ESP_OK;
/* FW-07 — gpio_get_level returns a level (0 or 1). Default 0 =
 * not pressed (active-LOW GPIO 0). */
static int      g_get_level_return   = 0;

static int g_config_count    = 0;
static int g_set_level_count = 0;
/* FW-07 — counter for gpio_get_level calls. */
static int g_get_level_count = 0;

static int g_last_pin    = -1;
static int g_last_level  = -1;
static int g_captured    = 0;
/* FW-07 — last pin + level seen by gpio_get_level. */
static int g_get_level_last_pin   = -1;
static int g_get_level_last_level = -1;

void mock_gpio_config_set_return(esp_err_t r)       { g_config_return   = r; }
void mock_gpio_set_level_set_return(esp_err_t r)    { g_set_level_return = r; }
/* FW-07 — prime the next gpio_get_level return value. */
void mock_gpio_get_level_set_return(int level)      { g_get_level_return = level ? 1 : 0; }

int mock_gpio_config_call_count(void)        { return g_config_count; }
int mock_gpio_set_level_call_count(void)     { return g_set_level_count; }
/* FW-07 — number of gpio_get_level calls observed. */
int mock_gpio_get_level_call_count(void)     { return g_get_level_count; }

void mock_gpio_set_level_capture(int *out_pin, int *out_level)
{
    if (out_pin)   *out_pin   = g_last_pin;
    if (out_level) *out_level = g_last_level;
}

/* FW-07 — last (pin, level) seen by gpio_get_level. */
void mock_gpio_get_level_capture_last(int *out_pin, int *out_level)
{
    if (out_pin)   *out_pin   = g_get_level_last_pin;
    if (out_level) *out_level = g_get_level_last_level;
}

void mock_gpio_reset(void)
{
    g_config_return    = ESP_OK;
    g_set_level_return = ESP_OK;
    g_get_level_return = 0;
    g_config_count     = 0;
    g_set_level_count  = 0;
    g_get_level_count  = 0;
    g_last_pin         = -1;
    g_last_level       = -1;
    g_captured         = 0;
    g_get_level_last_pin   = -1;
    g_get_level_last_level = -1;
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

/* FW-07 — gpio_get_level target. Records pin + level (the level
 * is whatever the test primed via mock_gpio_get_level_set_return,
 * default 0). Mirrors how mock_gpio_set_level captures the
 * write side. */
int mock_gpio_get_level(gpio_num_t pin)
{
    g_get_level_count++;
    g_get_level_last_pin   = (int)pin;
    g_get_level_last_level = g_get_level_return;
    return g_get_level_return;
}
