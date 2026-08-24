# CMakeLists.txt — ws component (FW-13; FW-16 server mode).
#
#   ws.c                  — server-mode init + sink seam plumbing
#   ws_server.c           — /cams WS endpoint (single inbound viewer)
#   ws_text_frame.c       — JSON builders (hello + status + URL helpers)
#   ws_event_handler.c    — retained outbound-client event handlers
#                           (unwired from the active path in FW-16)
#   ws_status_timer.c     — periodic esp_timer create/start/stop
#   ws_runtime_metrics.c  — getter fan-out (uptime/rssi/heap/drops/reconnects)
#   ws_reconnects.c       — reconnects_get() stub (retained)
#   ws_backoff.c          — retained FW-14 backoff (unwired in server mode)
#
# PRIV_REQUIRES esp_http_server — httpd_ws_send_frame_async +
# endpoint registration in ws_server.c; softap — the STA httpd
# handle accessor (one-directional edge: softap NEVER links ws,
# otherwise softap→ws→wifi→softap closes and static-link order
# breaks). The identity REQUIRES is for
# device_identity_t in the JSON builders. The esp_websocket_client
# managed component stays declared in idf_component.yml so the
# retained (unwired) sources keep compiling.
idf_component_register(
    SRCS "ws.c"
         "ws_server.c"
         "ws_text_frame.c"
         "ws_event_handler.c"
         "ws_status_timer.c"
         "ws_runtime_metrics.c"
         "ws_reconnects.c"
         "ws_backoff.c"
    INCLUDE_DIRS "include"
    REQUIRES identity wifi capture esp_websocket_client
    PRIV_REQUIRES esp_http_server softap
)