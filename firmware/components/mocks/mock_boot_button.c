/* mock_boot_button.c — host-side stub for the boot-button signal. */
#include "mock_boot_button.h"

static bool g_button_pressed = false;

void mock_boot_button_set(bool pressed) { g_button_pressed = pressed; }

bool mock_boot_button_pressed_at_boot_impl(void) { return g_button_pressed; }