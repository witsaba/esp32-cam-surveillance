# button CMakeLists.txt — FW-07 boot-button driver (R-03 measurement
# half + R-24 runtime factory reset).
#
# REQUIRES driver esp_timer config — pulls in the IDF GPIO driver
# (gpio_config / gpio_get_level) + esp_timer (esp_timer_create /
# esp_timer_get_time) + the project's `config` component (for the
# runtime factory-reset path wired at FW-07.3). The 2 new mocks
# (`mock_gpio_get_level` + `mock_esp_timer_get_time`) live under
# components/mocks/ and are linked by run_host_tests.py directly
# (not via this CMakeLists).
idf_component_register(
    SRCS "button.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer config
)
