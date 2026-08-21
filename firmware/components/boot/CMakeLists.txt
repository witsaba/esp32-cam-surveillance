idf_component_register(
    SRCS "boot.c"
         "boot_button_stub.c"
         "stub_inits.c"
         "stub_supervision.c"
    INCLUDE_DIRS "include"
    REQUIRES config nvs_flash
)