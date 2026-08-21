/* boot_button_stub.c — weak default for the boot-button signal.
 *
 * Lives in its OWN translation unit (not boot.c) so the symbol
 * survives -flto when no strong override exists yet. FW-07's
 * component provides the strong symbol that consults GPIO +
 * press duration; production must keep this TU linked until
 * FW-07 lands.
 *
 * On the host build (`UNITY_HOST_BUILD` defined), this TU is
 * skipped — `boot.c` pulls in `mock_boot_link.h` which redirects
 * `boot_button_pressed_at_boot` to the mock implementation, so
 * the linker complains about a duplicate symbol if both TUs are
 * active.
 */
#include "boot.h"

#ifndef UNITY_HOST_BUILD
__attribute__((weak)) bool boot_button_pressed_at_boot(void) {
    return false;
}
#endif