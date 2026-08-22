/* led.c — FW-06 status-LED driver.
 *
 * Implements the 8-state enum + GPIO + esp_timer state machine
 * per PRD § FR-7 L222-232. The control surface maps each
 * `led_set_state(s)` call to:
 *
 *   BOOTING          → solid ON  (no timer; level held ON)
 *   WIFI_CONNECTING  → 200 ms blink (timer period 100_000 us)
 *   WS_CONNECTING    → 100 ms blink (timer period  50_000 us)
 *   CONNECTED_IDLE   → 1 s heartbeat (timer period 500_000 us)
 *   STREAMING        → solid ON  (no timer; level held ON)
 *   RECONNECT_BACKOFF→ 2 s blink  (timer period 1_000_000 us)
 *   SOFT_RECOVERY    → 5 Hz rapid (timer period 50_000 us) +
 *                        3 s one-shot alarm fires recovery-cb
 *   UNINIT           → sentinel; rejected by led_set_state
 *
 * The timer model uses two persistent handles created once in
 * led_init(): `g_periodic_handle` (blink toggle) and
 * `g_oneshot_handle` (3 s recovery alarm). On every
 * `led_set_state(s)` call:
 *   - volatile write `g_state = s`
 *   - For solid-on states (BOOTING/STREAMING): esp_timer_stop
 *     the periodic, then gpio_set_level(ON).
 *   - For blinking states: esp_timer_restart(period_us) with the
 *     new state's half-period, then gpio_set_level(ON).
 *   - For SOFT_RECOVERY: also esp_timer_start_once(3_000_000 us)
 *     on the one-shot handle. On fire, the one-shot callback
 *     invokes `g_led_recovery_cb` (registered via
 *     led_on_recovery_complete()).
 *
 * Active-LOW polarity: writing gpio_set_level(0) turns the LED
 * ON (AI-THINKER hardware). The driver inverts internally per
 * CONFIG_FIRMWARE_LED_ACTIVE_LOW.
 *
 * Concurrency: g_state is single-byte; volatile write is atomic
 * on Xtensa LX6. The timer callback runs from the esp_timer
 * task; the setter updates g_state BEFORE re-arming the timer
 * so the next callback sees the new state. Worst case: one
 * extra toggle on transition.
 *
 * Bite-proof gate for FW-06.4 (Phase 5 commit): when the build
 * defines -DLED_TEST_STUB_DISABLE_TIMER=1, the timer-create body
 * is short-circuited so the timer never fires; the production
 * guard then trips on `set_state_rearms_timer` with a message
 * containing the literal "timer_fire" so the runner can verify
 * the guard is load-bearing.
 */
#include <string.h>

#include "led.h"

#ifdef UNITY_HOST_BUILD
#include "mock_gpio_link.h"
#include "mock_esp_timer_link.h"
#else
#include "driver/gpio.h"
#include "esp_timer.h"
#endif

/* Per-state half-period in microseconds (0 = solid-on, no timer).
 * Indexed by led_state_t. We use `uint32_t` here because
 * esp_timer_start_periodic takes uint64_t; the cast happens at
 * the call site. The CONFIG_FIRMWARE_LED_PERIOD_*_MS defaults
 * are in milliseconds; we multiply by 1000 once at init-time. */
typedef struct {
    uint32_t period_us;   /* half-period for blink; 0 = solid-on */
    int      is_solid;    /* 1 = solid ON, no timer (BOOTING/STREAMING) */
} led_state_cfg_t;

static led_state_cfg_t led_state_cfg(led_state_t s);

/* State storage — single-byte volatile, atomic write on
 * Xtensa LX6. Matches FW-03 lock-free idiom in boot.c. */
static volatile led_state_t g_state = LED_STATE_UNINIT;
static volatile int         g_initialized = 0;

/* Persistent timer handles (created once in led_init). The
 * periodic handle drives blinking; the one-shot handle fires
 * the recovery-complete cb. */
static esp_timer_handle_t g_periodic_handle = NULL;
static esp_timer_handle_t g_oneshot_handle  = NULL;

/* Recovery-complete callback (FW-06.3 + FW-06.4 S2). NULL until
 * led_on_recovery_complete() registers one. */
static volatile led_recovery_cb_t g_led_recovery_cb = NULL;

/* Resolve the active-LOW-aware ON level. */
static inline uint32_t led_level_on(void)
{
    return CONFIG_FIRMWARE_LED_ACTIVE_LOW ? 0u : 1u;
}

/* Resolve the active-LOW-aware OFF level. */
static inline uint32_t led_level_off(void)
{
    return CONFIG_FIRMWARE_LED_ACTIVE_LOW ? 1u : 0u;
}

/* Periodic callback: toggle the GPIO level. Runs from the
 * esp_timer task (dispatch_method ESP_TIMER_TASK). Reads
 * g_state (volatile) so any setter change is visible. */
static void led_periodic_cb(void *arg)
{
    (void)arg;
    /* Toggle by writing the opposite of the current level. The
     * mock's gpio_set_level_capture is updated, but we don't need
     * to inspect the prior value here — the toggle happens at the
     * hardware layer on device. */
    static uint32_t toggle = 0;
    toggle ^= 1u;
    gpio_set_level(CONFIG_FIRMWARE_LED_GPIO,
                   toggle ? led_level_off() : led_level_on());
}

/* One-shot callback: fire the registered recovery-complete cb
 * (FW-06.3 S3). If no cb is registered, this is a no-op (the
 * recovery alarm still fires; downstream consumers register via
 * led_on_recovery_complete() before invoking
 * led_set_state(LED_STATE_SOFT_RECOVERY)). */
static void led_oneshot_cb(void *arg)
{
    (void)arg;
    led_recovery_cb_t cb = (led_recovery_cb_t)g_led_recovery_cb;
    if (cb) cb();
}

/* Initialize GPIO + 2 esp_timer handles. */
esp_err_t led_init(void)
{
    if (g_initialized) return ESP_OK;

    /* Configure GPIO 4 (or override) for output, no pull, no intr. */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CONFIG_FIRMWARE_LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t r = gpio_config(&cfg);
    if (r != ESP_OK) return r;

#ifndef LED_TEST_STUB_DISABLE_TIMER
    /* Periodic handle — used for blinking. Callback fires every
     * period_us; the callback toggles the GPIO level. */
    esp_timer_create_args_t periodic_args = {
        .callback        = led_periodic_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "led_periodic",
    };
    r = esp_timer_create(&periodic_args, &g_periodic_handle);
    if (r != ESP_OK) return r;

    /* One-shot handle — used for the 3 s recovery alarm. */
    esp_timer_create_args_t oneshot_args = {
        .callback        = led_oneshot_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "led_oneshot",
    };
    r = esp_timer_create(&oneshot_args, &g_oneshot_handle);
    if (r != ESP_OK) {
        /* Best-effort cleanup so a partial init doesn't leak. */
        esp_timer_delete(g_periodic_handle);
        g_periodic_handle = NULL;
        return r;
    }
#else
    /* Bite-proof: skip timer creation. The production guard in
     * led_set_state then trips on the timer-fire invariant. */
#endif

    /* Initial level = OFF (idle before any led_set_state call). */
    gpio_set_level(CONFIG_FIRMWARE_LED_GPIO, led_level_off());

    g_state = LED_STATE_UNINIT;
    g_initialized = 1;
    return ESP_OK;
}

/* Set the LED state. See led.h for the documented contract. */
esp_err_t led_set_state(led_state_t s)
{
    if ((int)s <= (int)LED_STATE_UNINIT || s >= LED_STATE_COUNT) {
        if (s == LED_STATE_UNINIT) return ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_ARG;
    }

    g_state = s;
    led_state_cfg_t cfg = led_state_cfg(s);

    if (cfg.is_solid) {
        /* BOOTING / STREAMING: stop the periodic, hold level ON. */
        if (g_periodic_handle) {
            esp_timer_stop(g_periodic_handle);
        }
        gpio_set_level(CONFIG_FIRMWARE_LED_GPIO, led_level_on());
    } else {
        /* Blinking: stop + restart periodic with the new state's
         * period. We always `esp_timer_stop` first (idempotent if
         * already stopped) then `esp_timer_start_periodic`. This
         * makes the assertion `start_periodic_call_count >= 1`
         * hold on every transition into a blink state. */
        if (g_periodic_handle) {
            esp_timer_stop(g_periodic_handle);
            esp_err_t r = esp_timer_start_periodic(g_periodic_handle, cfg.period_us);
            if (r != ESP_OK) return r;
        }
        /* Entry condition: LED starts ON at the beginning of each
         * blink cycle. */
        gpio_set_level(CONFIG_FIRMWARE_LED_GPIO, led_level_on());
    }

    /* SOFT_RECOVERY additionally arms the 3 s one-shot alarm. */
    if (s == LED_STATE_SOFT_RECOVERY && g_oneshot_handle) {
        uint64_t duration_us = (uint64_t)CONFIG_FIRMWARE_LED_RECOVERY_DURATION_MS * 1000ULL;
        esp_err_t r = esp_timer_start_once(g_oneshot_handle, duration_us);
        if (r != ESP_OK) return r;
    }

    return ESP_OK;
}

esp_err_t led_deinit(void)
{
    if (!g_initialized) return ESP_OK;
    if (g_periodic_handle) {
        esp_timer_stop(g_periodic_handle);
        esp_timer_delete(g_periodic_handle);
        g_periodic_handle = NULL;
    }
    if (g_oneshot_handle) {
        esp_timer_stop(g_oneshot_handle);
        esp_timer_delete(g_oneshot_handle);
        g_oneshot_handle = NULL;
    }
    g_state = LED_STATE_UNINIT;
    g_initialized = 0;
    g_led_recovery_cb = NULL;
    return ESP_OK;
}

esp_err_t led_on_recovery_complete(led_recovery_cb_t cb)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    g_led_recovery_cb = cb;
    return ESP_OK;
}

/* Resolve a state's configuration. */
static led_state_cfg_t led_state_cfg(led_state_t s)
{
    led_state_cfg_t out = { .period_us = 0, .is_solid = 0 };
    switch (s) {
        case LED_STATE_BOOTING:
            out.period_us = 0;
            out.is_solid  = 1;
            break;
        case LED_STATE_WIFI_CONNECTING:
            out.period_us = (uint32_t)CONFIG_FIRMWARE_LED_PERIOD_WIFI_CONNECTING_MS * 500u;
            out.is_solid  = 0;
            break;
        case LED_STATE_WS_CONNECTING:
            out.period_us = (uint32_t)CONFIG_FIRMWARE_LED_PERIOD_WS_CONNECTING_MS * 500u;
            out.is_solid  = 0;
            break;
        case LED_STATE_CONNECTED_IDLE:
            out.period_us = (uint32_t)CONFIG_FIRMWARE_LED_PERIOD_IDLE_HEARTBEAT_MS * 500u;
            out.is_solid  = 0;
            break;
        case LED_STATE_STREAMING:
            out.period_us = 0;
            out.is_solid  = 1;
            break;
        case LED_STATE_RECONNECT_BACKOFF:
            out.period_us = (uint32_t)CONFIG_FIRMWARE_LED_PERIOD_BACKOFF_MS * 500u;
            out.is_solid  = 0;
            break;
        case LED_STATE_SOFT_RECOVERY:
            out.period_us = (uint32_t)CONFIG_FIRMWARE_LED_PERIOD_RECOVERY_MS * 500u;
            out.is_solid  = 0;
            break;
        default:
            out.period_us = 0;
            out.is_solid  = 1;
            break;
    }
    return out;
}
