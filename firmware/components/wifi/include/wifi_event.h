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

/* FW-08.6 — guard tripwire. When the build defines
 * `-DWIFI_TEST_STUB_SKIP_IP_UP_HANDLER=1`, wifi_event.c::
 * on_sta_got_ip_handler() is replaced by a no-op + a call to
 * this function. The body prints the literal "teardown" and
 * aborts via TEST_FAIL_MESSAGE. Pass 8 of run_host_tests.py
 * greps for the literal to confirm the guard is load-bearing.
 *
 * Not part of the production API — declared here only so the
 * wifi_event.c on_sta_got_ip_handler() body can call it
 * without a forward declaration. */
void wifi_event_guard_fail_teardown_on_ip_disabled(void);

#ifdef __cplusplus
}
#endif
