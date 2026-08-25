idf_component_register(
    SRCS "health.c"
         "health_window.c"
    INCLUDE_DIRS "include"
    # FW-16 — soft-recovery health task. PRIV_REQUIRES mirrors the
    # stream precedent (stream.c includes boot_priq.h): wifi for the
    # event-subscription seam, led for the SOFT_RECOVERY indication
    # + recovery-complete registration, boot for the supervision-step
    # enum + task stack/prio constants, esp_timer for the µs clock,
    # nvs_flash for the dedicated "recovery" namespace. The boot ↔
    # health mutual edge (boot REQUIRES health public; health
    # PRIV_REQUIRES boot) is proven by `idf.py build` at T3.
    PRIV_REQUIRES wifi led boot esp_timer nvs_flash
)
