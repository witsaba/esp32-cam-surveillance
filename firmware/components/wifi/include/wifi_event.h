/* wifi_event.h — event subscription surface for the wifi component.
 *
 * Mirrors IDF v5.5.3's esp_event_handler_t shape one-for-one so
 * production code can pass the IDF handler directly. The mock
 * (mock_esp_event_fire_handler) invokes the captured handler with
 * the same arity.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_EVT_STA_DISCONNECTED = 0,
    WIFI_EVT_STA_GOT_IP       = 1,
    WIFI_EVT_STA_CONNECTED    = 2,  /* reserved; counter reset uses GOT_IP */
} wifi_event_id_t;

/* Mirrors esp_event_handler_t (esp_event_base.h:22-24):
 *   void (*)(void *arg, esp_event_base_t base, int32_t id, void *data)
 *
 * On host, the esp_event_base_t is `const char *` (see
 * firmware/components/mocks/include/mock_esp_event.h). On device
 * it's a `typedef const char *esp_event_base_t`. The shape is
 * identical. */
typedef void (*wifi_event_cb_t)(void *arg,
                                 const char *event_base,
                                 int32_t event_id,
                                 void *event_data);

#ifdef __cplusplus
}
#endif
