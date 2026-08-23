/* ws_status_timer.c — periodic status-timer skeleton (FW-13,
 * T-13-C GREEN-only).
 *
 * T-13-C scope: stub out the API surface. The real
 * `esp_timer_create` (periodic, 30 s) + start/stop wiring +
 * callback lands in T-13-H GREEN.
 *
 * API contract:
 *   ws_status_timer_init() — create the periodic handle. Returns
 *     ESP_OK on the production path. On host, the mock layer
 *     captures the create call so a future test can verify the
 *     period.
 *   ws_status_timer_start() — arm the periodic timer.
 *   ws_status_timer_stop()  — disarm.
 *   ws_status_timer_handle_get() — expose the handle so tests can
 *     advance it via mock_esp_timer_advance_periodic().
 *
 * Today (T-13-C): all four functions are no-ops returning ESP_OK.
 */
#include "ws.h"

#include <string.h>

#ifdef UNITY_HOST_BUILD
#include "mock_esp_timer_link.h"
#else
#include "esp_timer.h"
#endif

esp_err_t ws_status_timer_init(void)
{
    /* T-13-H: real esp_timer_create(periodic, 30s) lands here. */
    return ESP_OK;
}

esp_err_t ws_status_timer_start(void)
{
    /* T-13-H: real esp_timer_start_periodic(g_status_timer,
     * CONFIG_FIRMWARE_WS_STATUS_PERIOD_MS * 1000) lands here. */
    return ESP_OK;
}

esp_err_t ws_status_timer_stop(void)
{
    /* T-13-H: real esp_timer_stop(g_status_timer) lands here. */
    return ESP_OK;
}

void *ws_status_timer_handle_get(void)
{
    /* T-13-H: returns the module-static esp_timer_handle_t. */
    return NULL;
}