/* button.h — FW-07 boot-button driver (R-03 + R-24).
 *
 * Public API for the AI-THINKER ESP32-CAM onboard PRG button
 * (GPIO 0, active-LOW per PRD § FR-7 L234-238). The driver owns
 * three responsibilities per PRD § FR-1 step 2 L237:
 *
 *   1. **Boot-time press signal** — `boot_button_pressed_at_boot()`
 *      returns true iff the user held the button for
 *      ≥ CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_LONGPRESS_MS (default
 *      3000 ms) during the BOOT_TIME phase. Consumed at FW-03
 *      boot.c:124 (UNCHANGED at FW-07 — the strong symbol here
 *      resolves via standard linker precedence over the weak
 *      default in `boot_button_stub.c`).
 *
 *   2. **Runtime long-press → factory reset** — fires a
 *      user-registered callback exactly once when the user holds
 *      the button for ≥ CONFIG_FIRMWARE_BOOT_BUTTON_RUNTIME_LONGPRESS_MS
 *      (default 10000 ms) during the RUNTIME phase. Production
 *      wires `config_factory_reset() + esp_restart()` into the
 *      callback (FW-07.3).
 *
 *   3. **Strap-pin tolerance** — the GPIO-0 ROM bootloader
 *      transient (~100 ms LOW after reset) is absorbed by a
 *      CONFIG_FIRMWARE_BOOT_BUTTON_STRAP_GRACE_MS (default 500 ms)
 *      window after `button_init()`. Polls before the window
 *      expires return without state change.
 *
 * Phase / state machines:
 *
 *   button_phase_t:
 *     STRAP_GRACE — ignore edges until `g_strap_release_us`
 *     BOOT_TIME   — measure boot-time press (until boot-longpress + 1 s slack)
 *     RUNTIME     — measure runtime press; fire cb if ≥10 s
 *
 *   button_state_t:
 *     IDLE     — no edge in flight
 *     PRESSED  — falling edge observed; waiting for release
 *     RELEASED — rising edge observed; press complete
 *
 * Concurrency: state storage is `volatile` (single-byte/8-byte
 * atomic writes on Xtensa LX6). The periodic esp_timer callback
 * runs from the esp_timer task (dispatch_method ESP_TIMER_TASK);
 * the public setters (`button_on_runtime_longpress_set`) write
 * the callback pointer atomically before the timer starts, so no
 * read/write race on the cb.
 *
 * Test entry: `button_poll_once(int64_t now_us)` advances the
 * state machine synchronously by one polling cycle. Production
 * invokes it via the esp_timer periodic callback; host tests call
 * it directly with a primed `mock_esp_timer_get_time()` value.
 *
 * Phase A status: this header is the public API surface only —
 * the implementation in `button.c` lands in Phase B-E.
 */
#ifndef BUTTON_H
#define BUTTON_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kconfig defaults (mirrors firmware/main/Kconfig.projbuild). The
 * host build does NOT compile Kconfig; the driver reads these
 * #defines which are set via the build's -DCONFIG_FIRMWARE_BOOT_BUTTON_*
 * flags. On device, Kconfig.projbuild emits CONFIG_FIRMWARE_BOOT_BUTTON_*
 * via menuconfig → sdkconfig.h. */
#ifndef CONFIG_FIRMWARE_BOOT_BUTTON_GPIO
#define CONFIG_FIRMWARE_BOOT_BUTTON_GPIO 0
#endif

#ifndef CONFIG_FIRMWARE_BOOT_BUTTON_TAP_MAX_MS
#define CONFIG_FIRMWARE_BOOT_BUTTON_TAP_MAX_MS 100
#endif

#ifndef CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_LONGPRESS_MS
#define CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_LONGPRESS_MS 3000
#endif

#ifndef CONFIG_FIRMWARE_BOOT_BUTTON_RUNTIME_LONGPRESS_MS
#define CONFIG_FIRMWARE_BOOT_BUTTON_RUNTIME_LONGPRESS_MS 10000
#endif

#ifndef CONFIG_FIRMWARE_BOOT_BUTTON_DEBOUNCE_MS
#define CONFIG_FIRMWARE_BOOT_BUTTON_DEBOUNCE_MS 20
#endif

#ifndef CONFIG_FIRMWARE_BOOT_BUTTON_STRAP_GRACE_MS
#define CONFIG_FIRMWARE_BOOT_BUTTON_STRAP_GRACE_MS 500
#endif

/* FW-07.2 — BOOT_TIME phase duration (microseconds). The state
 * machine is in BOOT_TIME from (button_init + STRAP_GRACE_MS)
 * to (button_init + STRAP_GRACE_MS + BOOT_TIME_WINDOW_MS).
 * Default 5000 ms gives a 5 s window for the user to assert
 * the provisioning signal; longer than BOOT_LONGPRESS_MS (3 s
 * default) plus 2 s of slack for the rising-edge measurement. */
#ifndef CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_TIME_WINDOW_MS
#define CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_TIME_WINDOW_MS 5000
#endif

typedef enum {
    BUTTON_PHASE_STRAP_GRACE = 0,  /* ignore edges until g_strap_release_us */
    BUTTON_PHASE_BOOT_TIME   = 1,  /* measure boot-time press */
    BUTTON_PHASE_RUNTIME     = 2,  /* measure runtime press; fire cb if ≥10s */
    BUTTON_PHASE_COUNT       = 3
} button_phase_t;

typedef enum {
    BUTTON_STATE_IDLE     = 0,
    BUTTON_STATE_PRESSED  = 1,
    BUTTON_STATE_RELEASED = 2,
    BUTTON_STATE_COUNT    = 3
} button_state_t;

/* Runtime long-press callback. Registered via
 * button_on_runtime_longpress_set(). Production wires
 * config_factory_reset() + esp_restart() (FW-07.3). Owner of
 * the registered callback: FW-15 (supervision) or FW-08
 * (wifi-station) — whichever boots first wires it. */
typedef void (*button_longpress_cb_t)(void);

/* Initialize the button driver: configures GPIO for input (with
 * internal pull-up) and creates the persistent 10 ms periodic
 * esp_timer handle. Idempotent — re-calling without an intervening
 * button_deinit() is a no-op returning ESP_OK.
 *
 * Returns ESP_OK on success, or the failing esp_err_t (without
 * side effects on partial success) on first failure. */
esp_err_t button_init(void);

/* Tear down the button driver: stops + deletes the periodic
 * esp_timer handle and resets phase to STRAP_GRACE + state to
 * IDLE. Idempotent. */
esp_err_t button_deinit(void);

/* Synchronous host-test entry: advance the state machine by one
 * polling cycle. now_us MUST be the value returned by
 * esp_timer_get_time() (or its mock equivalent). Production
 * callers invoke via the esp_timer periodic callback; host tests
 * call directly with a primed `mock_esp_timer_get_time()` value. */
void button_poll_once(int64_t now_us);

/* Register the runtime long-press callback. Stored until the next
 * RUNTIME-phase ≥ CONFIG_FIRMWARE_BOOT_BUTTON_RUNTIME_LONGPRESS_MS
 * press triggers exactly one invocation (cooldown
 * DEBOUNCE_MS * 4 = 80 ms prevents double-fire while the user
 * keeps holding). Returns ESP_OK; cb == NULL returns
 * ESP_ERR_INVALID_ARG. Re-registering replaces the previous
 * callback. Idempotent if the same non-NULL cb is re-registered. */
esp_err_t button_on_runtime_longpress_set(button_longpress_cb_t cb);

/* STRONG SYMBOL — overrides the weak default in
 * `boot_button_stub.c` via standard linker precedence. Returns
 * true iff the user held the button for
 * ≥ CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_LONGPRESS_MS during the
 * BOOT_TIME phase. Consumed by `boot.c:124` (FW-03.3 unchanged).
 * On host, `mock_boot_link.h` redirects this to
 * `mock_boot_button_get()` so the FW-03 determinism tests keep
 * their existing shape. */
bool boot_button_pressed_at_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_H */
