/* mock_boot_link.h — mirror of mock_nvs_flash_link.h for the
 * boot-button signal. On host builds, `boot_button_pressed_at_boot`
 * is redirected to the mock so tests can prime the return value
 * with `mock_boot_button_set(bool)`. On device builds
 * (`MOCK_BOOT_USE_REAL` is defined), the macro is a no-op and the
 * weak stub from `boot_button_stub.c` is the real symbol.
 */
#pragma once

#include "mock_boot_button.h"

#ifndef MOCK_BOOT_USE_REAL
#define boot_button_pressed_at_boot mock_boot_button_pressed_at_boot_impl
#endif