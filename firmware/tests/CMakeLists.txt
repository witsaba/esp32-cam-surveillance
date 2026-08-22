# Tests CMakeLists.txt — registers the test_config subdirectory.
#
# ESP-IDF Unity tests are components in their own right: each one
# declares its sources via `idf_component_register` and gets linked
# against the unity component. The test components REQUIRES the
# production components under test so the test binary has the same
# symbols as the production binary — and so the `mocks` component is
# pulled in (its mock_nvs_* symbols satisfy the redirected NVS calls
# inside the production source).
idf_component_register(
    SRCS ""
    INCLUDE_DIRS ""
)

add_subdirectory(test_config)
add_subdirectory(test_boot)
add_subdirectory(test_softap)
add_subdirectory(test_led)
add_subdirectory(test_button)