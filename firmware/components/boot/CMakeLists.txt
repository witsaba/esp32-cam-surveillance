idf_component_register(
    SRCS "boot.c"
         "boot_button_stub.c"
         "stub_inits.c"
         "stub_supervision.c"
    INCLUDE_DIRS "include"
    # stream (FW-15): boot.c calls stream_task_start — the real
    # strong symbol lives in components/stream (stub body deleted).
    REQUIRES config nvs_flash softap wifi camera capture ws stream
    PRIV_REQUIRES button
)