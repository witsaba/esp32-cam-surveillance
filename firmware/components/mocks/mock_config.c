/* mock_config.c — implementation of the FW-07.3 `config_factory_reset`
 * mock. Counter + return-slot pattern; mirrors `mock_esp_system.c`
 * for the `esp_restart()` line. */
#include "mock_config.h"

static int             g_factory_reset_count = 0;
static config_status_t g_factory_reset_ret   = CONFIG_OK;

void mock_config_factory_reset_set_return(config_status_t ret)
{
    g_factory_reset_ret = ret;
}

int mock_config_factory_reset_call_count(void)
{
    return g_factory_reset_count;
}

void mock_config_reset(void)
{
    g_factory_reset_count = 0;
    g_factory_reset_ret   = CONFIG_OK;
}

config_status_t mock_config_factory_reset(void)
{
    g_factory_reset_count++;
    return g_factory_reset_ret;
}
