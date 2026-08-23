# CMakeLists.txt — capture component (FW-11).
#
# Single TU (capture.c) + public header. Mirrors the
# FW-10 camera component shape. The esp32-camera managed-
# component dependency is NOT declared here (FW-06 gotcha:
# only firmware/main's idf_component.yml owns managed deps).
# The `REQUIRES camera` directive links against the
# in-tree camera component (which provides the sensor
# accessor seam) plus the boot mocks on host.
idf_component_register(
    SRCS "capture.c"
    INCLUDE_DIRS "include"
    REQUIRES camera
)
