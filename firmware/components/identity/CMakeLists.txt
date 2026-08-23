# CMakeLists.txt — identity component (FW-13).
#
# Leaf component. REQUIRES `nvs_flash` (for the nvs_open +
# nvs_get_str calls in identity.c). On host the mock layer
# supplies in-memory nvs_get_str; on device the IDF nvs_flash
# component is linked. The mocks are REQUIRES'd separately in
# the host build via the host_runner's all_sources list (not via
# this CMakeLists.txt — IDF doesn't build this component on
# host; the runner compiles identity.c directly).
idf_component_register(
    SRCS "identity.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)