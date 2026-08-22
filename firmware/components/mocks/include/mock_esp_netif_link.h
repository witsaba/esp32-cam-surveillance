/* mock_esp_netif_link.h — macro-redirect for the netif API. */
#pragma once

#include "mock_esp_netif.h"

#ifndef MOCK_NETIF_USE_REAL

#define esp_netif_init()                                mock_esp_netif_init()
#define esp_netif_create_default_wifi_ap()              mock_esp_netif_create_default_wifi_ap()
/* FW-08 — STA netif mock redirect. */
#define esp_netif_create_default_wifi_sta()            mock_esp_netif_create_default_wifi_sta()
#define esp_netif_set_default_netif(n)                  mock_esp_netif_set_default_netif(n)
#define esp_netif_destroy(h)                            mock_esp_netif_destroy(h)

#endif  /* !MOCK_NETIF_USE_REAL */
