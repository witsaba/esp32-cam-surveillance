/* ws_backoff.h — FW-14 reconnect-backoff module (R-19, FR-4).
 *
 * Owns the WS reconnect loop while `disable_auto_reconnect=true`
 * stays a hard invariant (the IDF built-in reconnect is disabled;
 * design #3805). On every DISCONNECTED/ERROR event the event
 * handler calls ws_backoff_on_failure(), which:
 *
 *   1. increments consecutive_failures,
 *   2. computes delay = CONFIG_FIRMWARE_WS_RECONNECT_INITIAL_MS ×
 *      2^(n−1), capped at CONFIG_FIRMWARE_WS_RECONNECT_CAP_MS,
 *   3. calls esp_websocket_client_set_reconnect_timeout(delay) per
 *      the FR-4 mandate (inert on device under the disable flag —
 *      upstream v1.8.0 returns ESP_ERR_INVALID_STATE; logged at
 *      debug. The module state below is authoritative),
 *   4. arms the one-shot esp_timer whose callback fires
 *      esp_websocket_client_start() — this IS the reconnect,
 *   5. logs the transition at WARN.
 *
 * Clean-CLOSE sleep latch (FW-14.3 guard): a CLOSED event with
 * close code 1000 latches sleep mode via ws_backoff_latch_set(true)
 * (which also cancels any pending reconnect timer). While latched,
 * the event handlers suppress scheduling until CONNECTED clears the
 * latch. FW-21 consumes the latch later — this module only sets/
 * clears it.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the one-shot reconnect timer handle (idempotent). Called
 * lazily by ws_backoff_on_failure(); safe to call from ws_init. */
void ws_backoff_init(void);

/* Pure FR-4 computation: delay for the nth consecutive failure =
 * INITIAL × 2^(n−1), capped at CAP. n == 0 is treated as 1. */
uint32_t ws_backoff_delay_ms(uint32_t consecutive_failures);

/* CONNECTED semantics: reset consecutive_failures to 0, clear the
 * clean-CLOSE latch, cancel any stale pending timer, and clear the
 * cached current delay. Wired into the WEBSOCKET_EVENT_CONNECTED
 * handler FIRST (before hello/status work). */
void ws_backoff_on_connected(void);

/* DISCONNECTED/ERROR semantics (call only when NOT latched):
 * increment the counter, compute + cache the delay, call the FR-4
 * setter, arm the one-shot reconnect timer, WARN-log the
 * transition. Returns the computed delay in ms. */
uint32_t ws_backoff_on_failure(void);

/* Delay computed by the most recent on_failure (0 before any). */
uint32_t ws_backoff_current_delay_ms(void);

/* Consecutive failures since the last CONNECTED/reset. */
uint32_t ws_backoff_failure_count(void);

/* Clean-CLOSE sleep latch. Setting it to true also cancels any
 * pending one-shot reconnect timer (latch invariant: while latched,
 * no reconnect is scheduled). Clearing is done by on_connected. */
void ws_backoff_latch_set(bool latched);
bool ws_backoff_latch_get(void);

/* Cancel the pending one-shot reconnect timer if armed. Safe when
 * the handle was never created. */
void ws_backoff_timer_cancel(void);

/* Host-test reset: zeroes counter/latch/delay and nulls the
 * module-static timer handle so the next init re-creates it in a
 * freshly cleared mock slot table. Call BEFORE mock_esp_timer_reset()
 * in test setups. Not for production use. */
void ws_backoff_reset_for_test(void);

#ifdef __cplusplus
}
#endif
