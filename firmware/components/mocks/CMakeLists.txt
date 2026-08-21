idf_component_register(
    SRCS "mock_nvs_flash.cpp"
         "mock_boot_button.c"
         "mock_init_returns.c"
         "mock_supervision_record.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash boot
    WHOLE_ARCHIVE
)