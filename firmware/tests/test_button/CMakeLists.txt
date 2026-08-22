# test_button CMakeLists.txt — Unity test cases for the FW-07 boot
# button driver. Mirrors the FW-02/FW-03/FW-05/FW-06 test_*
# CMakeLists pattern.
#
# On host, the runner (tools/run_host_tests.py) compiles the
# test files directly via gcc + IDF's unity.c — there is NO
# idf_component invocation. This file is included in the IDF
# test target for completeness and so an `idf.py test --target
# esp32` run also picks up the new tests (when the device is
# connected).
#
# PRIV_REQUIRES button mocks boot config unity — pulls in the
# production button component (under test) and the mocks
# component (whose mock_gpio_* + mock_esp_timer_* symbols
# satisfy the link-header redirects inside button.c). The
# `boot` component is required so the boot.h header is on the
# include path (the test does not link boot.c on host, but
# `mock_boot_link.h` redirects `boot_button_pressed_at_boot`
# to the mock).
idf_component_register(
    SRCS "test_button_tap_ignore.c"
         "test_button_boot_longpress.c"
         "test_button_runtime_reset.c"
         "test_button_guard.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES button mocks boot config unity
)
