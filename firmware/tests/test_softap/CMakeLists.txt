# test_softap CMakeLists.txt — Unity test cases for the FW-05 softap
# component. Mirrors the FW-02 test_config CMakeLists pattern.
#
# On host, the runner (tools/run_host_tests.py) compiles the test
# files directly via gcc + IDF's unity.c — there is NO idf_component
# invocation. This file is included in the IDF test target for
# completeness and so an `idf.py test --target esp32` run also picks
# up the new tests (when the device is connected).
#
# PRIV_REQUIRES softap config mocks unity — pulls in the production
# softap component (under test) and the mocks (mock_nvs_* for the
# config redirect; the four FW-05 mock surfaces are linked via the
# run_host_tests.py source list, not via this CMake).
idf_component_register(
    SRCS "test_softap_whoami.c"
         "test_softap_provision.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES softap config mocks unity
)
