# test_led CMakeLists.txt — Unity test cases for the FW-06 LED
# driver. Mirrors the FW-02/FW-03/FW-05 test_* CMakeLists pattern.
#
# On host, the runner (tools/run_host_tests.py) compiles the
# test files directly via gcc + IDF's unity.c — there is NO
# idf_component invocation. This file is included in the IDF
# test target for completeness and so an `idf.py test --target
# esp32` run also picks up the new tests (when the device is
# connected).
#
# PRIV_REQUIRES led mocks unity — pulls in the production led
# component (under test) and the mocks component (whose
# mock_gpio_* + mock_esp_timer_* symbols satisfy the link-header
# redirects inside led.c).
idf_component_register(
    SRCS "test_led_boot_connecting.c"
         "test_led_connected.c"
         "test_led_backoff_recovery.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES led mocks unity
)
# Note: test_led_guard.c is compiled only by run_host_tests.py Pass 5
# (with -DLED_TEST_STUB_DISABLE_TIMER=1). It is intentionally NOT
# compiled into the production host build because the bite-proof
# scenario requires the LED_TEST_STUB_DISABLE_TIMER gate.
