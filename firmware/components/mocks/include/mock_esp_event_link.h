/* mock_esp_event_link.h — macro-redirect for the IDF event loop API. */
#pragma once

#include "mock_esp_event.h"

#ifndef MOCK_EVENT_USE_REAL

#define esp_event_loop_create_default()                 mock_esp_event_loop_create_default()

#endif  /* !MOCK_EVENT_USE_REAL */
