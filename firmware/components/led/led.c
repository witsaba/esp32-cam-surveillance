/* led.c — FW-06 status-LED driver (placeholder).
 *
 * Phase 1 stub: all 4 entry points return ESP_OK. Phase 2
 * (FW-06.1) replaces this with the real implementation: state
 * table → esp_timer period, GPIO config via gpio_config,
 * periodic + one-shot handle management, recovery-cb dispatch.
 *
 * Bite-proof gate for FW-06.4 (timed for Phase 5 commit): when
 * the build defines -DLED_TEST_STUB_DISABLE_TIMER=1 (Pass 5 in
 * run_host_tests.py), the periodic timer creation body is
 * short-circuited so the timer "never fires" — the production
 * guard then trips on `set_state_rearms_timer` with a message
 * containing the literal "timer_fire" so the runner can verify
 * the guard is load-bearing.
 *
 * Concurrency note: state storage will be a single-byte
 * `volatile led_state_t`. Single-byte writes are atomic on
 * Xtensa LX6; matches the lock-free idiom in boot.c. The
 * setter updates g_state before re-arming the timer so the
 * next callback sees the new state. Worst case: one extra
 * toggle on transition (acceptable for a status LED).
 */
#include "led.h"

#ifdef LED_TEST_STUB_DISABLE_TIMER
/* FW-06.4 bite-proof: this branch intentionally breaks the
 * timer so the guard test trips. Real implementation will
 * gate the esp_timer_create body behind this same #ifdef. */
#endif

esp_err_t led_init(void)
{
    /* Placeholder: real impl will gpio_config + esp_timer_create
     * × 2 in Phase 2. */
    return ESP_OK;
}

esp_err_t led_set_state(led_state_t state)
{
    (void)state;
    return ESP_OK;
}

esp_err_t led_deinit(void)
{
    return ESP_OK;
}

esp_err_t led_on_recovery_complete(led_recovery_cb_t cb)
{
    (void)cb;
    return ESP_OK;
}
