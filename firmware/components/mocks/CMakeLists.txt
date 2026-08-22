idf_component_register(
    SRCS "mock_nvs_flash.cpp"
         "mock_boot_button.c"
         "mock_init_returns.c"
         "mock_supervision_record.c"
         "mock_log.c"
         "mock_esp_wifi.c"
         "mock_esp_netif.c"
         "mock_esp_event.c"
         "mock_http_server.c"
         "mock_esp_system.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash boot softap
    WHOLE_ARCHIVE
)