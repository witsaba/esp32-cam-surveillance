# test_config CMakeLists.txt — Unity test cases for the `config` module.
#
# `PRIV_REQUIRES config` ensures the test binary links against the
# production `config` component (the component under test) without
# exposing it as a public dependency. `PRIV_REQUIRES mocks` pulls in
# the mock_nvs_* symbols that the production source redirects to
# (via mock_nvs_flash_link.h).
idf_component_register(
    SRCS "test_config_load_fresh.c"
         "test_config_roundtrip.c"
         "test_config_schema_mismatch.c"
         "test_config_schema_persists.c"
         "test_config_guard.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES config mocks unity
)