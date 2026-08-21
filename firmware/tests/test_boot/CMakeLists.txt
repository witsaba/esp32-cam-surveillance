# test_boot CMakeLists.txt — Unity test cases for the `boot` module.
#
# PRIV_REQUIRES boot config mocks unity — links the test binary against
# the production `boot` component (the system under test) and the
# `config` + `mocks` components (boot depends on config; tests
# depend on the mocks for the redirection of boot_button + init returns
# + supervision record).
idf_component_register(
    SRCS "test_boot_order.c"
         "test_boot_fail_loud.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES boot config mocks unity
)