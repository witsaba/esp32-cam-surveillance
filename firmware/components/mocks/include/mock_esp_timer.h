/* mock_esp_timer.h — host-side mock for the IDF esp_timer.
 *
 * FW-06 status-LED exercises the IDF esp_timer API:
 *   esp_timer_create(&args, &out)     (×2 in led_init: periodic + oneshot)
 *   esp_timer_start_periodic(h, us)   (re-armed on every state transition)
 *   esp_timer_start_once(h, us)       (one-shot 3 s alarm for soft-recovery)
 *   esp_timer_stop(h)                 (when leaving a state)
 *   esp_timer_restart(h, us)          (alternative to stop+start_periodic)
 *   esp_timer_delete(h)               (in led_deinit)
 *
 * On host, the production source (led.c) includes
 * `mock_esp_timer_link.h` which `#define`s each production
 * symbol to the mock symbol below.
 *
 * Crucial test entry: `mock_esp_timer_fire_callback(handle)`
 * synchronously invokes the esp_timer_cb_t registered via
 * esp_timer_create for the given handle. Required for:
 *   - FW-06.3 S3 (recovery-complete cb fires after 3 s one-shot)
 *   - FW-06.4 bite-proof (verify the timer-fire invariant)
 *
 * Mirrors FW-05's mock_httpd_invoke_registered_handler pattern.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Forward-declared types matching IDF's esp_timer.h. On host
 * we don't pull in IDF. esp_timer_handle_t is opaque on
 * device; we back it with a small registry so the mock can
 * look up the registered callback for fire_callback(). */
typedef struct mock_esp_timer_handle *esp_timer_handle_t;

/* esp_timer_cb_t — same signature as IDF. */
typedef void (*esp_timer_cb_t)(void *arg);

/* esp_timer_dispatch_t — we only need the TASK value. */
typedef int esp_timer_dispatch_t;
#define ESP_TIMER_TASK 0

/* esp_timer_create_args_t — minimal stub. The mock inspects
 * only `callback` + `arg`; other fields are opaque. */
typedef struct {
    esp_timer_cb_t      callback;
    void               *arg;
    esp_timer_dispatch_t dispatch_method;
    const char         *name;
    int                 skip_unhandled_events;
} esp_timer_create_args_t;

/* ---------- primable return values ---------- */
void mock_esp_timer_create_set_return(esp_err_t r);
void mock_esp_timer_start_periodic_set_return(esp_err_t r);
void mock_esp_timer_start_once_set_return(esp_err_t r);
void mock_esp_timer_stop_set_return(esp_err_t r);
void mock_esp_timer_restart_set_return(esp_err_t r);
void mock_esp_timer_delete_set_return(esp_err_t r);

/* ---------- call counters ---------- */
int mock_esp_timer_create_call_count(void);
int mock_esp_timer_start_periodic_call_count(void);
int mock_esp_timer_start_once_call_count(void);
int mock_esp_timer_stop_call_count(void);
int mock_esp_timer_restart_call_count(void);
int mock_esp_timer_delete_call_count(void);

/* ---------- captured state ---------- */
/* Last period_us passed to start_periodic / restart (last wins). */
uint64_t mock_esp_timer_last_period_us(void);
/* Last timeout_us passed to start_once. */
uint64_t mock_esp_timer_last_period_us_oneshot(void);
/* Number of timer handles created. Tests use this to look up
 * the handle from `mock_esp_timer_handle_at(index)`. */
int mock_esp_timer_handle_count(void);
/* Return the i-th handle created (0-based). Returns NULL if
 * out of range. The handle is owned by the mock; tests do not
 * free it. */
esp_timer_handle_t mock_esp_timer_handle_at(int idx);

/* ---------- reset ---------- */
void mock_esp_timer_reset(void);

/* ---------- test entry: fire a registered callback ---------- */
/* Synchronously invokes the esp_timer_cb_t registered via
 * esp_timer_create for the matching handle. Returns ESP_OK if
 * the handle was found and the callback fired; ESP_ERR_NOT_FOUND
 * if no callback was registered for this handle. The callback's
 * return value is ignored (void).
 *
 * This is the **load-bearing** test entry that lets the host
 * runner advance the state machine without a real esp_timer
 * task. Mirrors FW-05's mock_httpd_invoke_registered_handler. */
esp_err_t mock_esp_timer_fire_callback(esp_timer_handle_t handle);

/* ---------- mock targets (link-header redirects) ---------- */
esp_err_t mock_esp_timer_create(const esp_timer_create_args_t *args,
                                  esp_timer_handle_t *out_handle);
esp_err_t mock_esp_timer_start_periodic(esp_timer_handle_t handle,
                                          uint64_t period_us);
esp_err_t mock_esp_timer_start_once(esp_timer_handle_t handle,
                                      uint64_t timeout_us);
esp_err_t mock_esp_timer_stop(esp_timer_handle_t handle);
esp_err_t mock_esp_timer_restart(esp_timer_handle_t handle,
                                    uint64_t timeout_us);
esp_err_t mock_esp_timer_delete(esp_timer_handle_t handle);
