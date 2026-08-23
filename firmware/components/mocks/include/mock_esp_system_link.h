/* mock_esp_system_link.h — macro-redirect for the esp_system API. */
#pragma once

#include "mock_esp_system.h"

#ifndef MOCK_SYSTEM_USE_REAL

#define esp_read_mac(mac, type)             mock_esp_read_mac(mac, type)
#define esp_chip_info(info)                 mock_esp_chip_info(info)
#define esp_get_idf_version()               mock_esp_get_idf_version()
#define esp_restart()                       mock_esp_restart()
/* FW-13 — esp_get_free_heap_size() redirect target. */
#define esp_get_free_heap_size()            mock_esp_get_free_heap_size()

#endif  /* !MOCK_SYSTEM_USE_REAL */
