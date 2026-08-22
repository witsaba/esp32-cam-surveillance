# led CMakeLists.txt — FW-06 status-LED driver (R-23).
#
# REQUIRES driver esp_timer — pulls in the IDF GPIO driver
# (gpio_config / gpio_set_level) + the esp_timer component
# (esp_timer_create / start_periodic / start_once / stop /
# restart / delete). The 2 new mocks (mock_gpio +
# mock_esp_timer) live under components/mocks/ and are linked
# by run_host_tests.py directly (not via this CMakeLists).
idf_component_register(
    SRCS "led.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)
