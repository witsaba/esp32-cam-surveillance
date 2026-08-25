/* led.h — FW-06 status-LED control surface.
 *
 * Public API for the AI-THINKER ESP32-CAM onboard status LED
 * (GPIO 4, active-LOW per PRD § FR-7 L222-232). Downstream
 * milestones (FW-08 wifi-connecting, FW-13 ws-connecting,
 * FW-15 backoff + soft-recovery, FW-16 streaming, FW-19 idle
 * heartbeat) call led_set_state(...) from their respective
 * call sites. FW-06 ships the API + state machine + timer; the
 * integration into boot/wifi/ws/stream/control lands in those
 * downstream milestones per the FW-06 charter.
 *
 * State machine (8 states; 7 per FR-7 + UNINIT sentinel):
 *
 *   BOOTING           → solid ON  (timer stopped, level held)
 *   WIFI_CONNECTING   → 200 ms blink (timer period 100_000 us)
 *   WS_CONNECTING     → 100 ms blink (timer period  50_000 us)
 *   CONNECTED_IDLE    → LED OFF (steady state; GPIO 4 is the
 *                        AI-Thinker flash LED, so a heartbeat here
 *                        wastes significant power — fault blinks in
 *                        the other states remain the diagnostic)
 *   STREAMING         → solid ON  (timer stopped, level held)
 *   RECONNECT_BACKOFF → 2 s blink (timer period 1_000_000 us)
 *   SOFT_RECOVERY     → 5 Hz rapid (timer period 50_000 us)
 *                        sustained 3 s via a one-shot alarm that
 *                        fires the registered recovery-complete cb
 *   UNINIT            → sentinel; rejected by led_set_state
 *                        (FW-06.4 invariant: UNINIT MUST NOT be
 *                        a settable target)
 *
 * Active-LOW polarity: writing gpio_set_level(0) turns the LED
 * ON. The driver inverts internally per FIRMWARE_LED_ACTIVE_LOW
 * (default y per AI-THINKER hardware).
 *
 * Concurrency: led_set_state is the only public setter; the
 * timer callback runs from the esp_timer task (dispatch_method
 * ESP_TIMER_TASK). State storage is `volatile led_state_t` and
 * is updated as a single-byte atomic write (Xtensa LX6). Worst
 * case: one extra toggle on transition (acceptable for a status
 * LED).
 */
#ifndef LED_H
#define LED_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kconfig defaults (mirrors firmware/main/Kconfig.projbuild). The
 * host build does NOT compile Kconfig; the driver reads these
 * #defines which are set via the build's -DCONFIG_FIRMWARE_LED_*
 * flags. On device, Kconfig.projbuild emits CONFIG_FIRMWARE_LED_*
 * via menuconfig → sdkconfig.h. */
#ifndef CONFIG_FIRMWARE_LED_GPIO
#define CONFIG_FIRMWARE_LED_GPIO 4
#endif

#ifndef CONFIG_FIRMWARE_LED_ACTIVE_LOW
#define CONFIG_FIRMWARE_LED_ACTIVE_LOW 0
#endif

#ifndef CONFIG_FIRMWARE_LED_PERIOD_WIFI_CONNECTING_MS
#define CONFIG_FIRMWARE_LED_PERIOD_WIFI_CONNECTING_MS 200
#endif
#ifndef CONFIG_FIRMWARE_LED_PERIOD_WS_CONNECTING_MS
#define CONFIG_FIRMWARE_LED_PERIOD_WS_CONNECTING_MS 100
#endif
#ifndef CONFIG_FIRMWARE_LED_PERIOD_IDLE_HEARTBEAT_MS
#define CONFIG_FIRMWARE_LED_PERIOD_IDLE_HEARTBEAT_MS 1000
#endif
#ifndef CONFIG_FIRMWARE_LED_PERIOD_BACKOFF_MS
#define CONFIG_FIRMWARE_LED_PERIOD_BACKOFF_MS 2000
#endif
#ifndef CONFIG_FIRMWARE_LED_PERIOD_RECOVERY_MS
#define CONFIG_FIRMWARE_LED_PERIOD_RECOVERY_MS 100
#endif
#ifndef CONFIG_FIRMWARE_LED_RECOVERY_DURATION_MS
#define CONFIG_FIRMWARE_LED_RECOVERY_DURATION_MS 3000
#endif

typedef enum {
    LED_STATE_UNINIT            = 0,
    LED_STATE_BOOTING           = 1,
    LED_STATE_WIFI_CONNECTING   = 2,
    LED_STATE_WS_CONNECTING     = 3,
    LED_STATE_CONNECTED_IDLE    = 4,
    LED_STATE_STREAMING         = 5,
    LED_STATE_RECONNECT_BACKOFF = 6,
    LED_STATE_SOFT_RECOVERY     = 7,
    LED_STATE_COUNT             = 8
} led_state_t;

/* Recovery-complete callback signature. Registered via
 * led_on_recovery_complete(); fired by the 3-second one-shot
 * alarm when the LED is in LED_STATE_SOFT_RECOVERY. Owner:
 * FW-16 (health registers its completion cb, which calls
 * esp_restart()). */
typedef void (*led_recovery_cb_t)(void);

/* Initialize the LED driver: configures the GPIO pin for output
 * and creates the persistent periodic + one-shot esp_timer
 * handles. Idempotent — re-calling without an intervening
 * led_deinit() is a no-op returning ESP_OK.
 *
 * Returns ESP_OK on success, or the failing esp_err_t (without
 * side effects on partial success) on first failure. */
esp_err_t led_init(void);

/* Set the LED state. Validates state ∈ (LED_STATE_UNINIT,
 * LED_STATE_COUNT). UNINIT returns ESP_ERR_INVALID_STATE
 * (FW-06.4 invariant: UNINIT MUST NOT be a settable target).
 * Out-of-range returns ESP_ERR_INVALID_ARG without touching
 * GPIO or timer.
 *
 * Re-arms the periodic esp_timer within ONE period of the
 * previous pattern on every transition (FW-06.4 invariant;
 * guarded by test_set_state_rearms_timer + the bite-proof in
 * test_led_guard.c).
 *
 * SOFT_RECOVERY additionally arms the one-shot alarm at
 * FIRMWARE_LED_RECOVERY_DURATION_MS; on fire the registered
 * recovery-complete cb is invoked. */
esp_err_t led_set_state(led_state_t state);

/* Tear down the LED driver: stops + deletes both timer handles
 * and resets state to UNINIT. Idempotent. */
esp_err_t led_deinit(void);

/* Register the recovery-complete callback. Stored until the
 * next LED_STATE_SOFT_RECOVERY one-shot fires (FW-06.3 S3).
 * Returns ESP_OK; cb == NULL returns ESP_ERR_INVALID_ARG. */
esp_err_t led_on_recovery_complete(led_recovery_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* LED_H */
