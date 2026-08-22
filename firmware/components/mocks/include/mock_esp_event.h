/* mock_esp_event.h — host-side mock for the IDF event loop driver.
 *
 * FW-05 softAP bring-up calls esp_event_loop_create_default() before
 * esp_netif_create_default_wifi_ap() and esp_wifi_init() — IDF v5.5.3
 * requires the default event loop to exist before the wifi driver can
 * deliver WIFI_EVENT_AP_START and friends (caught on device flash,
 * engram #3630). On host the mock returns whatever the test primed
 * via set_*_return_set() (default ESP_OK); the production source
 * already treats ESP_ERR_INVALID_STATE as success (idempotent init).
 *
 * FW-08 adds the wifi component's event-subscription surface:
 *   - esp_event_handler_instance_register_with: capture (base, id,
 *     handler, arg, instance) tuple for the mock fire_handler.
 *   - mock_esp_event_fire_handler(base, id, event_data): test entry
 *     that synchronously invokes the captured handler with the
 *     same arity as IDF's esp_event_handler_t. Mirrors the
 *     mock_esp_timer_fire_callback pattern at mock_esp_timer.c:205-211.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* On host we model esp_event_base_t as a `const char *` (the
 * production shape on device). The mock capture compares pointers
 * by literal address, so callers pass "WIFI_EVENT" / "IP_EVENT"
 * string literals that survive the test's lifetime. */
typedef const char *esp_event_base_t;

/* esp_event_handler_t shape (mirrors esp_event_base.h:22-24). */
typedef void (*esp_event_handler_t)(void *arg,
                                     esp_event_base_t base,
                                     int32_t event_id,
                                     void *event_data);

/* Instance handle — opaque. The mock tracks instances for
 * unregister counting; production code discards them. */
typedef void *esp_event_handler_instance_t;

/* ---------- primable return values ---------- */
void mock_esp_event_loop_create_default_return_set(esp_err_t r);
/* FW-08 — esp_event_handler_instance_register return prime (default-loop). */
void mock_esp_event_handler_instance_register_return_set(esp_err_t r);

/* ---------- call counters ---------- */
int  mock_esp_event_loop_create_default_call_count(void);
/* FW-08 — subscription counter + fire invocation counter. */
int  mock_esp_event_handler_instance_register_call_count(void);
int  mock_esp_event_fire_handler_call_count(void);

/* ---------- reset ---------- */
void mock_esp_event_reset(void);

/* ---------- test entry: fire the captured handler ----------
 *
 * Synchronously invokes the captured handler with the supplied
 * (base, id, event_data) — exactly the arity of the IDF
 * esp_event_handler_t. Linear scan over the capture table for
 * the first matching (base, id); returns ESP_ERR_NOT_FOUND if no
 * match (mirrors mock_esp_timer_fire_callback's not-found
 * semantics). The handler is invoked as
 * captured.handler(captured.arg, base, id, event_data). */
esp_err_t mock_esp_event_fire_handler(esp_event_base_t base,
                                       int32_t event_id,
                                       void *event_data);

/* ---------- capture inspection (FW-08) ----------
 *
 * mock_esp_event_registered_count returns the number of active
 * (base, id) subscriptions. mock_esp_event_last_captured_handler /
 * _arg return the handler/arg of the most recent subscription
 * registered via esp_event_handler_instance_register_with — used
 * by tests that drive events through the mock and want to
 * inspect what was wired up. */
int  mock_esp_event_registered_count(void);
void mock_esp_event_last_captured_handler(esp_event_handler_t *out_handler,
                                            void **out_arg);
void mock_esp_event_last_captured_base_id(esp_event_base_t *out_base,
                                            int32_t *out_id);

/* ---------- mock targets ---------- */
esp_err_t mock_esp_event_loop_create_default(void);
/* FW-08 — default-loop instance register (matches real IDF
 * esp_event_handler_instance_register at
 * components/esp_event/default_event_loop.c:28-44 — 5 args, no
 * event_loop param, internally uses s_default_loop). */
esp_err_t mock_esp_event_handler_instance_register(
    esp_event_base_t base,
    int32_t event_id,
    esp_event_handler_t handler,
    void *arg,
    esp_event_handler_instance_t *instance);
