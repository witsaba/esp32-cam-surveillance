# CMakeLists.txt — ws component (FW-13).
#
# 4-file split per design #3756 §1:
#   ws.c                  — init orchestrator + lazy start
#   ws_text_frame.c       — JSON builders (hello + status + URL)
#   ws_event_handler.c    — IDF event subscriptions + handlers
#   ws_status_timer.c     — periodic esp_timer create/start/stop
#   ws_runtime_metrics.c  — getter fan-out (uptime/rssi/heap/drops/reconnects)
#   ws_reconnects.c       — reconnects_get() stub (FW-14 owns the real one)
#
# REQUIRES capture — for capture_fb_drops_get() in the status payload
# reader (T-13-I). The wifi REQUIRES is for wifi_event_subscribe in
# ws_event_handler.c (the IP-up subscription per design smell #4).
# The identity REQUIRES is for device_identity_t in the JSON builders.
# The esp_websocket_client managed component is declared in
# idf_component.yml (transferred from main/idf_component.yml in T-13-C).
idf_component_register(
    SRCS "ws.c"
         "ws_text_frame.c"
         "ws_event_handler.c"
         "ws_status_timer.c"
         "ws_runtime_metrics.c"
         "ws_reconnects.c"
         "ws_backoff.c"
    INCLUDE_DIRS "include"
    REQUIRES identity wifi capture esp_websocket_client
)