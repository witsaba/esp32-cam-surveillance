/* mock_softap_link.h — macro-redirect for the softap component.
 *
 * On host (UNITY_HOST_BUILD), every softap_* call site is swapped
 * for the mock_* symbol. Production source includes this BEFORE
 * <softap.h>. Mirrors the mock_esp_wifi_link.h / mock_esp_timer_
 * link.h pattern. */
#pragma once

#include "mock_softap.h"

#ifndef MOCK_SOFTAP_USE_REAL

#define softap_stop()                    mock_softap_stop()
#define softap_is_active()               mock_softap_is_active_get()

#endif  /* !MOCK_SOFTAP_USE_REAL */
