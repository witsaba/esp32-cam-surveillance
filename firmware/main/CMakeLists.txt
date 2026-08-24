idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES boot config nvs_flash log camera capture stream
)