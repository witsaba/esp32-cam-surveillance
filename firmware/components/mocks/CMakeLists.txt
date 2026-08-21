idf_component_register(
    SRCS "mock_nvs_flash.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
    WHOLE_ARCHIVE
)