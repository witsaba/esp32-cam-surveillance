idf_component_register(
    SRCS "boot.c"
         "boot_button_stub.c"
         "stub_inits.c"
         "stub_supervision.c"
    INCLUDE_DIRS "include"
    # stream (FW-15): boot.c calls stream_task_start — the real
    # strong symbol lives in components/stream (stub body deleted).
    # health (FW-16): boot.c calls health_task_start — the real
    # strong symbol lives in components/health (stub body deleted;
    # the boot↔health mutual priv/public REQUIRES edge is proven
    # by `idf.py build`).
    REQUIRES config nvs_flash softap wifi camera capture ws stream health
    PRIV_REQUIRES button
)