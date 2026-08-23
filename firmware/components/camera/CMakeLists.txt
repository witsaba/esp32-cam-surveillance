# CMakeLists.txt — camera component (FW-10).
#
# Single component, two TUs (camera.c + camera_settings.c) —
# mirrors the FW-08 wifi component shape. The esp32-camera
# managed-component dependency is NOT declared here (FW-06 gotcha:
# only firmware/main's idf_component.yml owns managed deps). The
# `REQUIRES esp32-camera` directive is the build-time link to the
# managed component's CMake target.
idf_component_register(
    SRCS "camera.c"
         "camera_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32-camera boot esp_psram
)
