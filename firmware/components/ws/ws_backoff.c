/* ws_backoff.c — FW-14 reconnect-backoff implementation (R-19, FR-4).
 *
 * See ws_backoff.h for the module contract. Design #3805 decisions
 * encoded here:
 *   - disable_auto_reconnect stays true; this module owns the loop
 *     via a one-shot esp_timer whose callback calls
 *     esp_websocket_client_start().
 *   - esp_websocket_client_set_reconnect_timeout() is called per the
 *     FR-4 mandate even though upstream v1.8.0 rejects it under the
 *     disable flag (returns ESP_ERR_INVALID_STATE) — logged at debug;
 *     module state is authoritative.
 *   - Delay table: INITIAL × 2^(n−1) capped at CAP
 *     (defaults 2000/4000/8000/16000/30000/30000).
 */
#include "ws_backoff.h"
#include "ws.h"

#ifdef UNITY_HOST_BUILD
#include "mock_esp_websocket_client_link.h"
#include "mock_esp_timer_link.h"
#else
#include "esp_websocket_client.h"
#include "esp_timer.h"
#endif

#include "esp_log.h"

static const char *TAG = "ws_backoff";

/* Module-static state. */
static uint32_t s_consecutive_failures = 0;
static uint32_t s_current_delay_ms     = 0;
static bool     s_sleep_latched        = false;

/* One-shot reconnect timer. Created lazily by ws_backoff_init()
 * and reused for every arm cycle (mirrors ws_status_timer). */
static esp_timer_handle_t s_reconnect_timer = NULL;

/* The reconnect itself: fire esp_websocket_client_start on the
 * current handle. The IDF client re-enters its connecting state
 * asynchronously; if no handle exists yet (pre-init failure path)
 * there is nothing to restart. */
static void ws_backoff_reconnect_cb(void *arg)
{
    (void)arg;
    esp_websocket_client_handle_t h = ws_handle_get();
    if (!h) {
        ESP_LOGW(TAG, "reconnect timer fired without ws handle");
        return;
    }
    esp_err_t r = esp_websocket_client_start(h);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "reconnect start failed: %s", esp_err_to_name(r));
    } else {
        ESP_LOGI(TAG, "reconnect fired after backoff (%u ms)",
                 (unsigned)s_current_delay_ms);
    }
}

void ws_backoff_init(void)
{
    if (s_reconnect_timer != NULL) return;

    esp_timer_create_args_t args = {
        .callback = ws_backoff_reconnect_cb,
        .arg      = NULL,
        .name     = "ws_backoff",
    };
    esp_err_t r = esp_timer_create(&args, &s_reconnect_timer);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(r));
        s_reconnect_timer = NULL;
    }
}

uint32_t ws_backoff_delay_ms(uint32_t consecutive_failures)
{
    uint64_t delay = (uint64_t)CONFIG_FIRMWARE_WS_RECONNECT_INITIAL_MS;
    const uint64_t cap = (uint64_t)CONFIG_FIRMWARE_WS_RECONNECT_CAP_MS;

    uint32_t doublings = consecutive_failures > 0 ? consecutive_failures - 1 : 0;
    for (uint32_t i = 0; i < doublings; ++i) {
        delay *= 2;
        if (delay >= cap) {
            return (uint32_t)cap;
        }
    }
    if (delay > cap) {
        delay = cap;
    }
    return (uint32_t)delay;
}

uint32_t ws_backoff_on_failure(void)
{
    s_consecutive_failures++;
    s_current_delay_ms = ws_backoff_delay_ms(s_consecutive_failures);

    /* FR-4 mandate: publish the schedule through the IDF setter.
     * Under disable_auto_reconnect=true upstream v1.8.0 rejects
     * with ESP_ERR_INVALID_STATE without writing state — expected;
     * debug-level note only. */
    esp_websocket_client_handle_t h = ws_handle_get();
    if (h) {
        esp_err_t sr = esp_websocket_client_set_reconnect_timeout(
            h, (int)s_current_delay_ms);
        ESP_LOGD(TAG, "set_reconnect_timeout(%u ms) -> %s",
                 (unsigned)s_current_delay_ms, esp_err_to_name(sr));
    }

    ESP_LOGW(TAG, "ws failure #%u; reconnect in %u ms",
             (unsigned)s_consecutive_failures,
             (unsigned)s_current_delay_ms);

    /* Arm the one-shot reconnect timer. Stop first so a re-arm on
     * an already-pending timer restarts the window cleanly. */
    if (s_reconnect_timer == NULL) {
        ws_backoff_init();
    }
    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
        esp_err_t r = esp_timer_start_once(
            s_reconnect_timer, (uint64_t)s_current_delay_ms * 1000ULL);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "esp_timer_start_once failed: %s",
                     esp_err_to_name(r));
        }
    }

    return s_current_delay_ms;
}

uint32_t ws_backoff_current_delay_ms(void)
{
    return s_current_delay_ms;
}

uint32_t ws_backoff_failure_count(void)
{
    return s_consecutive_failures;
}

void ws_backoff_latch_set(bool latched)
{
    s_sleep_latched = latched;
    if (latched) {
        /* Latch invariant: while latched, no reconnect is pending. */
        ws_backoff_timer_cancel();
    }
}

bool ws_backoff_latch_get(void)
{
    return s_sleep_latched;
}

void ws_backoff_timer_cancel(void)
{
    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
}

void ws_backoff_reset_for_test(void)
{
    s_consecutive_failures = 0;
    s_current_delay_ms     = 0;
    s_sleep_latched        = false;
    /* Null WITHOUT deleting: mock_esp_timer_reset() wipes the slot
     * table in tests; the stale pointer must not be reused. */
    s_reconnect_timer = NULL;
}
