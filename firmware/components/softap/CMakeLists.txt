# softap CMakeLists.txt — softAP provisioning HTTP surface (FW-05).
#
# REQUIRES config esp_wifi esp_netif esp_http_server esp_event —
# pulls in the IDF Wi-Fi + netif + HTTP server + event-loop
# components, plus the in-tree `config` component (config_t +
# config_save). The cJSON managed-component dependency is declared
# via idf_component.yml; component-manager resolves it from the
# top-level manifest and adds it to the build graph automatically.
idf_component_register(
    SRCS "softap.c"
         "softap_handlers.c"
    INCLUDE_DIRS "include"
    REQUIRES boot config esp_wifi esp_netif esp_http_server esp_event
)
