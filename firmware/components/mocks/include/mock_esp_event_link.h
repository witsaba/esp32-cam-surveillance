/* mock_esp_event_link.h — macro-redirect for the IDF event loop API. */
#pragma once

#include "mock_esp_event.h"

#ifndef MOCK_EVENT_USE_REAL

#define esp_event_loop_create_default()                 mock_esp_event_loop_create_default()
/* FW-08 — instance-based handler register. */
#define esp_event_handler_instance_register_with(b, id, h, arg, inst) \
        mock_esp_event_handler_instance_register_with(b, id, h, arg, inst)
/* IDF v5.5.3 event base constants — needed by FW-08 tests. */
#define WIFI_EVENT "WIFI_EVENT"
#define IP_EVENT   "IP_EVENT"
#define WIFI_EVENT_STA_DISCONNECTED 5
#define WIFI_EVENT_STA_CONNECTED    4
#define IP_EVENT_STA_GOT_IP         0

#endif  /* !MOCK_EVENT_USE_REAL */
