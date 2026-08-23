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
         "mock_gpio.c"
         "mock_esp_timer.c"
         "mock_config.c"
         "mock_softap.c"
         "mock_esp_camera.c"
         "mock_esp_websocket_client.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash boot softap driver esp_timer config
    WHOLE_ARCHIVE
)