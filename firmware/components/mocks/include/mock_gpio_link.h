/* mock_gpio_link.h — macro-redirect for the GPIO API.
 *
 * Mirrors mock_esp_wifi_link.h: every gpio_* call site is
 * replaced by the mock_* symbol below when MOCK_GPIO_USE_REAL
 * is NOT defined. Production source (led.c) includes this
 * BEFORE <driver/gpio.h>.
 *
 * On host (UNITY_HOST_BUILD defined), the macros are active.
 * On device, the macros are inactive and the real gpio_*
 * symbols from the IDF esp_driver_gpio component are linked.
 */
#pragma once

#include "mock_gpio.h"

#ifndef MOCK_GPIO_USE_REAL

#define gpio_config(cfg)             mock_gpio_config(cfg)
#define gpio_set_level(pin, level)   mock_gpio_set_level(pin, level)

#endif  /* !MOCK_GPIO_USE_REAL */
