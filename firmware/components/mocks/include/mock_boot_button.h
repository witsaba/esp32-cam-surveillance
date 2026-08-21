/* mock_boot_button.h — host-side stub for the boot-button signal.
 *
 * Primed by `mock_boot_button_set(bool)`. Read by the macro-redirected
 * `boot_button_pressed_at_boot()` call inside `boot.c` (the redirect
 * fires through `mock_boot_link.h` when the test build is active).
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void mock_boot_button_set(bool pressed);
bool mock_boot_button_pressed_at_boot_impl(void);

#ifdef __cplusplus
}
#endif