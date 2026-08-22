/* mock_config_link.h — macro-redirect for the FW-07.3
 * `config_factory_reset()` mock.
 *
 * Mirrors the `mock_esp_system_link.h` / `mock_nvs_flash_link.h`
 * pattern: when a production TU `#include`s this header (only
 * under `UNITY_HOST_BUILD` — production source guards it with
 * `#ifdef UNITY_HOST_BUILD`), every `config_factory_reset()` call
 * resolves to the `mock_config_factory_reset()` symbol after
 * preprocessing.
 *
 * The macro is gated by `#ifndef MOCK_CONFIG_USE_REAL`. On
 * device builds the macro is NOT defined and the real
 * `config_factory_reset()` from `config.c` resolves normally.
 */
#pragma once

#include "mock_config.h"

#ifndef MOCK_CONFIG_USE_REAL

#define config_factory_reset()  mock_config_factory_reset()

#endif  /* !MOCK_CONFIG_USE_REAL */
