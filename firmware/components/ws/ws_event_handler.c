/* ws_event_handler.c — WS event handler registration + bodies.
 * (FW-13, T-13-D GREEN; FW-14 adds ERROR + CLOSED + reconnect
 * scheduling.)
 *
 * Five handlers + one installer:
 *   ws_event_handler_on_sta_got_ip        — fires
 *     esp_websocket_client_start (lazy start per FW-13.1).
 *   ws_event_handler_on_ws_connected      — resets the FW-14 backoff
 *     counter + latch FIRST, then emits hello + starts the status
 *     timer.
 *   ws_event_handler_on_ws_disconnected   — stops status timer;
 *     unless clean-CLOSE-latched, schedules the FW-14 reconnect via
 *     ws_backoff_on_failure() (WARN transition).
 *   ws_event_handler_on_ws_error          — identical semantics to
 *     DISCONNECTED (ERROR-event counting parity, R-19).
 *   ws_event_handler_on_ws_closed         — close code 1000 latches
 *     the sleep latch (cancelling any pending reconnect); other
 *     codes are a no-op.
 *   ws_event_handler_install()             — idempotent installer;
 *     wires the handlers to their event sources.
 *
 * On host: the wifi_event_subscribe surface routes to
 * mock_esp_event_handler_instance_register (mock_esp_event_link.h
 * redirect). The ws_event_handler_install() function is what
 * ws.c calls — keeps the cross-component surface thin.
 */
#include "ws.h"

#include <string.h>

#ifdef UNITY_HOST_BUILD
#include "mock_esp_event_link.h"
#include "mock_esp_websocket_client_link.h"
#include "mock_esp_timer_link.h"
#include "wifi_event.h"
#include "wifi.h"
#include "esp_event.h"
#else
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include "wifi_event.h"
#include "wifi.h"
#endif

#include "ws_backoff.h"

#include "esp_log.h"

static const char *TAG = "ws_event";

/* Module-static state. The ws_handle is set by ws.c::ws_init_impl
 * (via ws_handle_set()) and read by the IP-up handler. */
static esp_websocket_client_handle_t s_ws_handle = NULL;

/* Idempotency: ws_event_handler_install() returns immediately if
 * already installed (mirrors the wifi component's idempotency
 * guard for backoff timer creation). The boot orchestrator calls
 * ws_init exactly once, but host tests reset mocks between tests
 * and may re-trigger the install path; the guard prevents
 * duplicate event-handler subscriptions from corrupting the
 * mock capture table. */
static bool s_event_handlers_installed = false;

/* Test-only reset. Clears the idempotency flag so the next
 * ws_init_impl → ws_event_handler_install() call re-registers
 * the handlers (the mock resets its handler table on each
 * _init call, so the production idempotency would otherwise
 * leave the new test with no handlers registered).
 * Declared in ws.h for host test access. */
void ws_event_handler_reset_for_test(void)
{
    s_event_handlers_installed = false;
}

/* Accessors used by ws.c to pass the handle to the event
 * handlers. ws_event_handler_install reads s_ws_handle and binds
 * the CONNECTED/DISCONNECTED subscriptions to it. */
void ws_handle_set(esp_websocket_client_handle_t h)
{
    s_ws_handle = h;
}

/* ---------- handler bodies ---------- */

/* IP_EVENT_STA_GOT_IP — fire esp_websocket_client_start (lazy
 * start). This is the only call to _start in production. The
 * mock fires CONNECTED synchronously inside _start, which then
 * chains into ws_event_handler_on_ws_connected (T-13-E GREEN
 * lands the hello emit). */
void ws_event_handler_on_sta_got_ip(void *arg,
                                     const char *event_base,
                                     int32_t event_id,
                                     void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    if (s_ws_handle == NULL) {
        ESP_LOGW(TAG, "sta_got_ip fired before ws_init set handle; "
                       "ignoring");
        return;
    }
    esp_err_t r = esp_websocket_client_start(s_ws_handle);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start failed: %s",
                 esp_err_to_name(r));
    }
}

/* WEBSOCKET_EVENT_CONNECTED — resets the FW-14 backoff state
 * FIRST (counter + sleep latch), then emits hello (T-13-E GREEN)
 * + starts status timer (T-13-H GREEN). The reset MUST precede
 * everything else so a reconnect that succeeds never inherits the
 * previous session's failure count. */
void ws_event_handler_on_ws_connected(void *handler_arg,
                                       esp_event_base_t base,
                                       int32_t event_id,
                                       void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)event_id;
    (void)event_data;

    /* FW-14.2: counter → 0, clean-CLOSE latch cleared, stale
     * pending reconnect timer cancelled. */
    ws_backoff_on_connected();

    esp_websocket_client_handle_t h = ws_handle_get();
    if (!h) {
        ESP_LOGE(TAG, "on_ws_connected: NULL handle; skipping hello");
        return;
    }

    /* Load identity (eFuse MAC + NVS name/description).
     * identity_load logs ESP_LOGW if NVS keys are missing but
     * always populates the MAC — the hello frame can still emit
     * with empty name/description. */
    device_identity_t id;
    esp_err_t r = identity_load(&id);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "on_ws_connected: identity_load failed: %s; "
                       "skipping hello", esp_err_to_name(r));
        return;
    }

    /* Build the hello JSON + send as the first text frame.
     * Per REQ-WS-002 S2 the hello MUST be the first text frame
     * (text opcode 0x1, not binary). The timeout
     * pdMS_TO_TICKS(500) matches the production cadence per
     * #3751 API correction. */
    char buf[256];
    size_t len = ws_text_frame_build_hello(&id, buf, sizeof(buf));
    if (len == 0) {
        ESP_LOGE(TAG, "on_ws_connected: ws_text_frame_build_hello "
                       "returned 0; skipping send");
        return;
    }
    int sent = esp_websocket_client_send_text(h, buf, (int)len,
#ifdef UNITY_HOST_BUILD
                                               /* Host has no FreeRTOS — the
                                                * mock ignores timeout_ticks;
                                                * pass 0 to skip the
                                                * pdMS_TO_TICKS macro. */
                                               0
#else
                                               pdMS_TO_TICKS(500)
#endif
                                               );
    if (sent < 0) {
        ESP_LOGE(TAG, "on_ws_connected: hello send failed: %d", sent);
    } else {
        ESP_LOGI(TAG, "hello sent: %d bytes (mac=%s name=%s)",
                 sent, id.mac_hex, id.name);
    }

    /* T-13-H: arm the 30 s status timer. The cadence test
     * (REQ-WS-005 S1) asserts exactly 3 status frames fire per
     * 90 s while connected. */
    esp_err_t tmr = ws_status_timer_start();
    if (tmr != ESP_OK) {
        ESP_LOGE(TAG, "on_ws_connected: ws_status_timer_start "
                       "failed: %s", esp_err_to_name(tmr));
    }
}

/* Shared failure path for DISCONNECTED + ERROR (FW-14): stop the
 * status timer, then — unless the clean-CLOSE sleep latch holds —
 * increment the counter and arm the one-shot reconnect timer via
 * ws_backoff_on_failure(). The WARN-level transition log lives in
 * the backoff module. */
static void ws_failure_stop_and_schedule(void)
{
    esp_err_t r = ws_status_timer_stop();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "ws_status_timer_stop failed: %s",
                 esp_err_to_name(r));
    }

#ifndef WS_TEST_STUB_ENABLE_CLOSE_RECONNECT
    /* FW-14.3 clean-CLOSE latch: while latched, no reconnect is
     * scheduled until CONNECTED clears it. Pass-12's bite-proof
     * stub build (-DWS_TEST_STUB_ENABLE_CLOSE_RECONNECT=1)
     * compiles this check OUT to prove the guard is load-bearing
     * ("close_no_reconnect"). */
    if (ws_backoff_latch_get()) {
        ESP_LOGI(TAG, "clean close latched; suppressing reconnect "
                       "(sleep invariant)");
        return;
    }
#endif

    (void)ws_backoff_on_failure();
}

/* WEBSOCKET_EVENT_DISCONNECTED — status frames suspended per
 * REQ-WS-005 S2; FW-14 owns the reconnect producer. */
void ws_event_handler_on_ws_disconnected(void *handler_arg,
                                          esp_event_base_t base,
                                          int32_t event_id,
                                          void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)event_id;
    (void)event_data;
    ws_failure_stop_and_schedule();
}

/* WEBSOCKET_EVENT_ERROR — identical counting/scheduling semantics
 * to DISCONNECTED (ERROR-event parity, R-19/FR-4). */
void ws_event_handler_on_ws_error(void *handler_arg,
                                   esp_event_base_t base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)handler_arg;
    (void)base;
    (int32_t)event_id;
    (void)event_data;
    ESP_LOGW(TAG, "ws transport error");
    ws_failure_stop_and_schedule();
}

/* WEBSOCKET_EVENT_CLOSED — derive cleanliness from the close
 * status code carried on the event payload (the pinned v1.8.0
 * component populates event_data.close_status_code on every
 * dispatch; it has no get_close_code accessor). Code 1000 latches
 * the sleep latch — which cancels any pending reconnect timer.
 * Any other code (or a NULL payload) is a no-op: the failure path
 * schedules normally. */
void ws_event_handler_on_ws_closed(void *handler_arg,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)event_id;

    const esp_websocket_event_data_t *ev =
        (const esp_websocket_event_data_t *)event_data;
    int close_code = ev ? ev->close_status_code : 0;

    if (close_code == 1000) {
        ESP_LOGI(TAG, "clean CLOSE (1000); latching sleep mode");
        ws_backoff_latch_set(true);
    } else {
        ESP_LOGD(TAG, "CLOSE with code %d; not a clean close", close_code);
    }
}

/* ---------- installer ---------- */

/* Idempotent installer. Subscribes:
 *   1. WIFI_EVT_STA_GOT_IP   → ws_event_handler_on_sta_got_ip
 *   2. WEBSOCKET_EVENT_CONNECTED     → ws_event_handler_on_ws_connected
 *      WEBSOCKET_EVENT_DISCONNECTED  → ws_event_handler_on_ws_disconnected
 *      WEBSOCKET_EVENT_ERROR         → ws_event_handler_on_ws_error (FW-14)
 *      WEBSOCKET_EVENT_CLOSED        → ws_event_handler_on_ws_closed (FW-14)
 *
 * ws.c::ws_init_impl calls this after esp_websocket_client_init +
 * esp_websocket_register_events. On host the wifi subscription
 * captures the (base, id) tuple via mock_esp_event_handler_
 * instance_register; on device it goes through the IDF default
 * event loop. */
esp_err_t ws_event_handler_install(void)
{
    if (s_event_handlers_installed) return ESP_OK;

    esp_err_t r = wifi_event_subscribe(WIFI_EVT_STA_GOT_IP,
                                        (wifi_event_cb_t)
                                          ws_event_handler_on_sta_got_ip,
                                        NULL);
    if (r != ESP_OK) return r;

    /* Register CONNECTED + DISCONNECTED handlers on the WS
     * handle. ws.c calls this AFTER esp_websocket_register_events
     * (which subscribes the handlers to the WS event loop) — but
     * the mock captures per-handler registration independently
     * of the WS handle subscription, so we re-register here for
     * the host-side test surface (one subscription per (event_id,
     * cb) tuple). On device the esp_websocket_register_events
     * call inside ws.c is the actual subscription; this local
     * call is a no-op on device (the WS event loop is a private
     * loop, not the IDF default loop). */
    r = esp_websocket_register_events(s_ws_handle,
                                       WEBSOCKET_EVENT_CONNECTED,
                                       (esp_event_handler_t)
                                         ws_event_handler_on_ws_connected,
                                       NULL);
    if (r != ESP_OK) return r;

    r = esp_websocket_register_events(s_ws_handle,
                                       WEBSOCKET_EVENT_DISCONNECTED,
                                       (esp_event_handler_t)
                                         ws_event_handler_on_ws_disconnected,
                                       NULL);
    if (r != ESP_OK) return r;

    /* FW-14 — ERROR parity + CLOSED latch subscription. */
    r = esp_websocket_register_events(s_ws_handle,
                                       WEBSOCKET_EVENT_ERROR,
                                       (esp_event_handler_t)
                                         ws_event_handler_on_ws_error,
                                       NULL);
    if (r != ESP_OK) return r;

    r = esp_websocket_register_events(s_ws_handle,
                                       WEBSOCKET_EVENT_CLOSED,
                                       (esp_event_handler_t)
                                         ws_event_handler_on_ws_closed,
                                       NULL);
    if (r != ESP_OK) return r;

    s_event_handlers_installed = true;
    return ESP_OK;
}
