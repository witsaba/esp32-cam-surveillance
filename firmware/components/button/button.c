/* button.c — FW-07 boot-button driver (R-03 measurement half + R-24
 * runtime factory reset). Phase C implementation: FW-07.2
 * boot-time long-press detection (latch + BOOT_TIME window +
 * strap-grace transient absorption).
 *
 * The driver owns 3 responsibilities per PRD § FR-1 step 2 L237 +
 * § FR-7 L234-238:
 *
 *   1. **Boot-time press signal** (FW-07.2 — Phase C, this file)
 *      — `boot_button_pressed_at_boot()` returns true iff the user
 *      held the button for ≥ BOOT_LONGPRESS_MS during the
 *      BOOT_TIME phase. The latch is sticky: once asserted it
 *      stays asserted until `button_deinit()` or a reboot.
 *
 *   2. **Runtime long-press → factory reset** (FW-07.3 — Phase D)
 *      — fires a user-registered callback exactly once when the
 *      user holds the button for ≥ RUNTIME_LONGPRESS_MS during
 *      the RUNTIME phase. Phase C declares
 *      `button_on_runtime_longpress_set()` to store the cb
 *      pointer; the actual dispatch lands in Phase D. The RUNTIME
 *      branch in `button_poll_once()` is a no-op in Phase C
 *      (state machine stays in whatever sub-state it had at the
 *      BOOT_TIME→RUNTIME transition; Phase D wires the cb fire).
 *
 *   3. **Strap-pin tolerance** (FW-07.1 — Phase B, this file) —
 *      the GPIO-0 ROM bootloader transient (~100 ms LOW after
 *      reset) is absorbed by a STRAP_GRACE_MS window after
 *      `button_init()`. Polls before the window expires return
 *      without state change.
 *
 * State machine (Phase C — FW-07.1 + FW-07.2):
 *
 *   phase: STRAP_GRACE → BOOT_TIME → RUNTIME
 *   state: IDLE → PRESSED → IDLE
 *
 *   STRAP_GRACE: ignore GPIO reads.
 *   BOOT_TIME: measure boot-time press. Latch
 *     `g_boot_button_pressed_at_boot` when duration crosses
 *     BOOT_LONGPRESS_MS.
 *   RUNTIME: stop measuring boot-time (Phase C). Phase D wires
 *     the runtime long-press cb dispatch here.
 *
 *   IDLE + falling edge → PRESSED; g_press_start_us = now_us
 *   PRESSED + rising edge:
 *     duration = now_us - g_press_start_us
 *     if duration ≤ TAP_MAX_US:  → IDLE (TAP — ignore)
 *     else if phase == BOOT_TIME + duration ≥ BOOT_LONGPRESS_MS:
 *                                  latch g_boot_button_pressed_at_boot
 *                                  → IDLE
 *     else:                      → IDLE (qualified press;
 *                                  Phase D will dispatch the
 *                                  runtime cb in the RUNTIME
 *                                  branch before reaching this
 *                                  path)
 *   PRESSED + each poll (while held): check duration; if
 *     duration ≥ BOOT_LONGPRESS_MS and phase == BOOT_TIME, latch
 *     g_boot_button_pressed_at_boot (sticky; stays set until
 *     button_deinit). This handles the S6 scenario where the
 *     press continues past BOOT_LONGPRESS_MS without a rising
 *     edge inside BOOT_TIME.
 *
 *   All polls while phase == STRAP_GRACE: return without state
 *   change.
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
 */
#include "button.h"

#include <stddef.h>

#include "esp_err.h"

#ifdef UNITY_HOST_BUILD
#include "mock_gpio_link.h"
#include "mock_esp_timer_link.h"
#else
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* Polling period (microseconds). Fixed at 10 ms for the lifetime
 * of the driver; matches the design's edge-latency budget (≤10
 * ms is well below the 100 ms tap threshold). */
#define BUTTON_POLL_PERIOD_US  (10 * 1000)

/* Resolve CONFIG_FIRMWARE_BOOT_BUTTON_* into microseconds for
 * the comparison. Done at init-time so the per-poll arithmetic
 * is a single subtraction + comparison. */
static inline int64_t button_tap_max_us(void)
{
    return (int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_TAP_MAX_MS * 1000LL;
}

static inline int64_t button_strap_grace_us(void)
{
    return (int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_STRAP_GRACE_MS * 1000LL;
}

/* FW-07.2 — boot-time long-press threshold (microseconds).
 * Compared against `now_us - g_press_start_us` while the state
 * machine is in BUTTON_PHASE_BOOT_TIME. Crossing this threshold
 * latches `g_boot_button_pressed_at_boot` sticky-true. */
static inline int64_t button_boot_longpress_us(void)
{
    return (int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_LONGPRESS_MS * 1000LL;
}

/* FW-07.2 — BOOT_TIME phase duration (microseconds). The state
 * machine is in BOOT_TIME from STRAP_GRACE end to STRAP_GRACE
 * end + BOOT_TIME_WINDOW. Default 5 s gives the user enough
 * slack to hold for ≥ BOOT_LONGPRESS_MS (3 s default) and still
 * see the rising edge inside BOOT_TIME; longer presses (10 s,
 * 30 s, etc.) cross into RUNTIME while the latch is already
 * sticky-true. */
static inline int64_t button_boot_time_window_us(void)
{
    return (int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_BOOT_TIME_WINDOW_MS * 1000LL;
}

/* Phase / state storage — single-byte/8-byte volatile, atomic
 * write on Xtensa LX6. Matches FW-03 lock-free idiom in boot.c. */
static volatile button_phase_t g_phase = BUTTON_PHASE_COUNT;  /* sentinel: uninit */
static volatile button_state_t g_state = BUTTON_STATE_IDLE;

/* Press-window timestamps. g_press_start_us is set on the
 * falling edge (IDLE → PRESSED); g_press_end_us is the
 * now_us of the most recent rising edge (PRESSED → IDLE).
 * g_last_edge_us feeds the debounce filter (Phase E).
 *
 * g_strap_release_us is the absolute now_us threshold below
 * which polls return without state change. Set once in
 * button_init() to (now + STRAP_GRACE_MS).
 *
 * g_boot_time_end_us is the absolute now_us threshold above
 * which polls transition from BOOT_TIME to RUNTIME. Set once
 * in button_init() to (now + STRAP_GRACE_MS + BOOT_TIME_WINDOW_MS).
 *
 * g_press_start_us uses a dedicated boolean sentinel
 * (g_press_in_progress) to detect "no press in flight" vs
 * "press started at us=0" — the latter can happen if the
 * falling edge lands on the very first poll inside
 * STRAP_GRACE. Using 0 as a sentinel would lose that case. */
static volatile int64_t g_strap_release_us = 0;
static volatile int64_t g_boot_time_end_us = 0;
static volatile int64_t g_press_start_us    = 0;
static volatile int64_t g_last_edge_us      = 0;
static volatile bool    g_press_in_progress = false;

/* BOOT_TIME long-press latch (FW-07.2 — Phase C). Sticky: once
 * asserted during BOOT_TIME (via the duration-crossing check
 * in button_poll_once()), it stays asserted until
 * `button_deinit()` clears it. Read by
 * `boot_button_pressed_at_boot()`. */
static volatile bool g_boot_button_pressed_at_boot = false;

/* Initialization guard. button_init() is idempotent. */
static volatile int g_initialized = 0;

/* Runtime long-press callback. NULL until the production wiring
 * (FW-07.3) registers one via button_on_runtime_longpress_set().
 * Phase D invokes this; Phase B stores the pointer but does NOT
 * invoke it (the tap-ignore state machine returns to IDLE on
 * every rising edge without consulting the cb). */
static volatile button_longpress_cb_t g_runtime_longpress_cb = NULL;

/* Persistent timer handle (created once in button_init).
 * Production: started in button_init(), stopped + deleted in
 * button_deinit(). On host, the mock's handle-count resets
 * via mock_esp_timer_reset() between tests. */
static esp_timer_handle_t g_poll_handle = NULL;

/* Forward declarations of the FreeRTOS / esp_timer callbacks
 * implemented below. Both the production esp_timer cb and the
 * FreeRTOS task wrapper are defined in the `#ifndef
 * UNITY_HOST_BUILD` block at the bottom of the file; on host,
 * stub definitions satisfy the linker so the test build links
 * cleanly. The test never invokes the cb — it calls
 * `button_poll_once()` directly to advance the state machine. */
#ifdef UNITY_HOST_BUILD
static void button_esp_timer_cb(void *arg);
#else
static void button_esp_timer_cb(void *arg);
static void button_poll_task(void *arg);
#endif

/* Initialize the button driver: configures GPIO 0 as input with
 * pull-up (active-LOW per PRD § FR-7 L234), creates the 10 ms
 * periodic esp_timer handle, sets the strap-grace release time,
 * and launches the polling task (production only).
 *
 * Idempotent — re-calling without an intervening
 * `button_deinit()` is a no-op returning ESP_OK. */
esp_err_t button_init(void)
{
    if (g_initialized) return ESP_OK;

    /* Configure GPIO for input + pull-up + no interrupt. The
     * PRG button on AI-THINKER ESP32-CAM is active-LOW with an
     * external pull-up; the internal pull-up is enabled as a
     * belt-and-braces measure for boards that omit the external
     * resistor. Polling (no interrupt) keeps
     * `gpio_install_isr_service` free for FW-08/FW-15. */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)CONFIG_FIRMWARE_BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,  /* enable internal pull-up */
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t r = gpio_config(&cfg);
    if (r != ESP_OK) return r;

    /* Strap-grace release time = now + STRAP_GRACE_MS. Polls
     * arriving before this timestamp return without state
     * change. esp_timer_get_time() is microsecond resolution.
     *
     * BOOT_TIME end = STRAP_GRACE release + BOOT_TIME_WINDOW.
     * Polls arriving after this timestamp transition the phase
     * machine from BOOT_TIME to RUNTIME. */
    int64_t now_us = esp_timer_get_time();
    g_strap_release_us = now_us + button_strap_grace_us();
    g_boot_time_end_us = g_strap_release_us + button_boot_time_window_us();
    g_last_edge_us    = now_us;

    /* Create the 10 ms periodic esp_timer handle. The callback
     * is `button_esp_timer_cb`; it forwards to button_poll_once
     * with the esp_timer_get_time() value. */
    esp_timer_create_args_t poll_args = {
        .callback        = button_esp_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "button_poll",
    };
    r = esp_timer_create(&poll_args, &g_poll_handle);
    if (r != ESP_OK) {
        g_poll_handle = NULL;
        return r;
    }
    r = esp_timer_start_periodic(g_poll_handle, (uint64_t)BUTTON_POLL_PERIOD_US);
    if (r != ESP_OK) {
        esp_timer_delete(g_poll_handle);
        g_poll_handle = NULL;
        return r;
    }

    g_phase = BUTTON_PHASE_STRAP_GRACE;
    g_state = BUTTON_STATE_IDLE;
    g_press_start_us      = 0;
    g_press_in_progress   = false;
    g_boot_button_pressed_at_boot = false;
    g_initialized         = 1;

#ifndef UNITY_HOST_BUILD
    /* Production: launch the polling task as well. On host
     * (UNITY_HOST_BUILD), the test calls button_poll_once()
     * directly so no task is needed. The FreeRTOS task wraps
     * the same esp_timer_cb → button_poll_once path; on
     * device the esp_timer task is the actual call site. */
    BaseType_t tr = xTaskCreate(button_poll_task, "button_poll",
                                 4096, NULL, 5, NULL);
    if (tr != pdPASS) {
        esp_timer_stop(g_poll_handle);
        esp_timer_delete(g_poll_handle);
        g_poll_handle = NULL;
        g_initialized = 0;
        return ESP_ERR_NO_MEM;
    }
#endif

    return ESP_OK;
}

/* Tear down the button driver: stops + deletes the periodic
 * esp_timer handle, resets state to IDLE, clears the runtime
 * callback. Idempotent. */
esp_err_t button_deinit(void)
{
    if (!g_initialized) return ESP_OK;
    if (g_poll_handle) {
        esp_timer_stop(g_poll_handle);
        esp_timer_delete(g_poll_handle);
        g_poll_handle = NULL;
    }
    g_phase          = BUTTON_PHASE_COUNT;  /* sentinel: uninit */
    g_state          = BUTTON_STATE_IDLE;
    g_press_start_us = 0;
    g_last_edge_us   = 0;
    g_strap_release_us = 0;
    g_boot_time_end_us = 0;
    g_press_in_progress = false;
    g_boot_button_pressed_at_boot = false;
    g_runtime_longpress_cb = NULL;
    g_initialized    = 0;
    return ESP_OK;
}

/* Synchronous state-machine advance (host-test entry + the
 * esp_timer cb body). now_us MUST be the value returned by
 * esp_timer_get_time() (or its mock equivalent).
 *
 * Phase C (FW-07.2) logic:
 *   - If now_us < g_strap_release_us: read it (to detect
 *     falling edges inside the strap window) but return
 *     without state-machine transitions. The press-start time
 *     IS recorded so a press that began inside STRAP_GRACE is
 *     measured from the actual falling edge, not from the
 *     BOOT_TIME entry.
 *   - Phase transition STRAP_GRACE → BOOT_TIME on the first
 *     poll after the grace window. If the GPIO is LOW at the
 *     transition moment, treat it as a press that started
 *     inside STRAP_GRACE (g_press_start_us was already set
 *     by the strap-grace polls).
 *   - Phase transition BOOT_TIME → RUNTIME on the first poll
 *     after g_boot_time_end_us.
 *   - Read the GPIO level (mocked on host).
 *   - Detect edges: falling (HIGH→LOW) opens a press window;
 *     rising (LOW→HIGH) closes it.
 *   - While state == PRESSED and phase == BOOT_TIME, check on
 *     each poll whether the press duration has crossed
 *     BOOT_LONGPRESS_MS; if so, latch
 *     `g_boot_button_pressed_at_boot = true` (sticky).
 *   - On the rising edge, compute duration:
 *       - If duration ≤ TAP_MAX_US: tap — discard.
 *       - If phase == BOOT_TIME and duration ≥ BOOT_LONGPRESS_MS:
 *         latch `g_boot_button_pressed_at_boot` (rising-edge
 *         catch in case the in-POLL check missed it — e.g. the
 *         duration crossed exactly at the rising edge boundary).
 *         Return to IDLE.
 *       - Else: qualified press — return to IDLE. Phase D will
 *         dispatch the runtime cb in the RUNTIME branch before
 *         reaching this path. */
void button_poll_once(int64_t now_us)
{
    if (!g_initialized) return;

    /* Strap-grace window: still read the GPIO so we capture
     * a falling edge that started before the window ended.
     * The state-machine transitions are deferred until the
     * window expires; once it does, transition to BOOT_TIME
     * and process this same poll as the first BOOT_TIME poll. */
    if (g_phase == BUTTON_PHASE_STRAP_GRACE) {
        int sg_level = gpio_get_level(CONFIG_FIRMWARE_BOOT_BUTTON_GPIO);
        bool sg_pressed_now = (sg_level == 0);
        /* Track falling + rising edges inside the strap-grace
         * window so a press that began BEFORE the window
         * ended is measured from the actual falling edge
         * (not from the BOOT_TIME entry). This is the
         * FW-07.2 contract: "hold for 3 seconds" must be
         * measured from when the user actually pressed it.
         * Rising edges clear the in-progress flag so a
         * subsequent falling edge inside BOOT_TIME can
         * start a fresh press measurement. */
        if (sg_pressed_now && !g_press_in_progress) {
            g_press_start_us = now_us;
            g_press_in_progress = true;
        } else if (!sg_pressed_now && g_press_in_progress) {
            /* Rising edge during strap grace: the user
             * released before the window ended. Clear the
             * in-progress flag so a later falling edge in
             * BOOT_TIME starts fresh. */
            g_press_in_progress = false;
            g_press_start_us = 0;
        }
        if (now_us < g_strap_release_us) return;
        g_phase = BUTTON_PHASE_BOOT_TIME;
        g_state = BUTTON_STATE_IDLE;
        /* Don't return — process this same poll as the first
         * BOOT_TIME poll so the strap-grace release doesn't
         * drop one edge. If the GPIO is still LOW at the
         * transition, fall through to the IDLE→PRESSED
         * branch below. */
    }

    /* BOOT_TIME → RUNTIME transition: first poll after
     * g_boot_time_end_us moves us into RUNTIME. From then on,
     * the boot-time latch is sticky (already-set stays set) and
     * no further boot-time measurement happens. Phase D wires
     * the runtime long-press cb dispatch on the RUNTIME
     * branch. */
    if (g_phase == BUTTON_PHASE_BOOT_TIME &&
        now_us >= g_boot_time_end_us) {
        g_phase = BUTTON_PHASE_RUNTIME;
        /* Stay in the current sub-state (IDLE or PRESSED).
         * A press that started in BOOT_TIME and crosses into
         * RUNTIME while still held keeps g_press_start_us so
         * Phase D can continue the duration measurement from
         * the original falling edge. */
    }

    int level = gpio_get_level(CONFIG_FIRMWARE_BOOT_BUTTON_GPIO);
    /* active-LOW: level == 0 means pressed. */
    bool pressed_now = (level == 0);

    switch (g_state) {
    case BUTTON_STATE_IDLE:
        if (pressed_now) {
            /* Falling edge: open a press window. If the
             * press started inside STRAP_GRACE,
             * g_press_in_progress + g_press_start_us were
             * already set there; if not (the press started
             * inside BOOT_TIME), record it now. */
            if (!g_press_in_progress) {
                g_press_start_us = now_us;
                g_press_in_progress = true;
            }
            g_state         = BUTTON_STATE_PRESSED;
            g_last_edge_us   = now_us;
        }
        break;
    case BUTTON_STATE_PRESSED:
        /* While still held in BOOT_TIME, check whether the
         * press duration has crossed BOOT_LONGPRESS_MS; if
         * so, latch the boot-time signal sticky-true. The
         * latch is idempotent — once set, additional polls
         * just re-write `true`. This handles the S6 scenario
         * (10 s press) where the duration keeps growing past
         * BOOT_LONGPRESS_MS without a rising edge. */
        if (pressed_now && g_phase == BUTTON_PHASE_BOOT_TIME) {
            int64_t held_us = now_us - g_press_start_us;
            if (held_us >= button_boot_longpress_us()) {
                g_boot_button_pressed_at_boot = true;
            }
        }
        if (!pressed_now) {
            /* Rising edge: close the press window. */
            int64_t duration_us = now_us - g_press_start_us;
            if (duration_us > button_tap_max_us()) {
                /* Qualified press. If we're still in
                 * BOOT_TIME and the press crossed
                 * BOOT_LONGPRESS_MS, latch the boot-time
                 * signal. This catches the rising-edge
                 * boundary case where the duration equals
                 * BOOT_LONGPRESS_MS exactly at the moment
                 * of release. */
                if (g_phase == BUTTON_PHASE_BOOT_TIME &&
                    duration_us >= button_boot_longpress_us()) {
                    g_boot_button_pressed_at_boot = true;
                }
                /* Phase D will dispatch the runtime cb in
                 * the RUNTIME branch before reaching this
                 * path. */
            }
            g_state       = BUTTON_STATE_IDLE;
            g_press_in_progress = false;  /* close the press window */
            g_press_start_us = 0;
            g_last_edge_us = now_us;
        }
        break;
    case BUTTON_STATE_RELEASED:
    default:
        /* Defensive: RELEASED is unused in Phase C (rising
         * edge collapses to IDLE). Reset to IDLE on any
         * unexpected state. */
        g_state = BUTTON_STATE_IDLE;
        break;
    }
}

/* Register the runtime long-press callback. Phase B stores
 * the pointer only; Phase D invokes it from the rising-edge
 * branch when a qualified ≥ RUNTIME_LONGPRESS_MS press
 * completes during the RUNTIME phase. cb == NULL returns
 * ESP_ERR_INVALID_ARG. */
esp_err_t button_on_runtime_longpress_set(button_longpress_cb_t cb)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    g_runtime_longpress_cb = cb;
    return ESP_OK;
}

/* STRONG SYMBOL — overrides the weak default in
 * `boot_button_stub.c` via standard linker precedence. On
 * host, `mock_boot_link.h` redirects this to
 * `mock_boot_button_pressed_at_boot_impl` so the boot orchestrator
 * tests keep their existing shape (the mock wins on host
 * for callers that include mock_boot_link.h; tests that
 * `#undef boot_button_pressed_at_boot` after the include
 * hit THIS strong symbol and verify the actual latch).
 *
 * Phase C (FW-07.2): returns true iff the user held the
 * button for ≥ BOOT_LONGPRESS_MS during the BOOT_TIME phase.
 * The latch is sticky — once asserted it stays asserted
 * until `button_deinit()` clears it. Consumed at boot.c:124
 * (FW-03.3 unchanged). */
bool boot_button_pressed_at_boot(void)
{
    return g_boot_button_pressed_at_boot;
}

#ifndef UNITY_HOST_BUILD
/* Production-only: the esp_timer periodic callback that
 * advances the state machine. Reads the wall clock from
 * esp_timer_get_time() and forwards to the synchronous
 * button_poll_once() entry. */
static void button_esp_timer_cb(void *arg)
{
    (void)arg;
    int64_t now_us = esp_timer_get_time();
    button_poll_once(now_us);
}

/* Production-only: the FreeRTOS task wrapper. The actual
 * state-machine driver is the esp_timer periodic callback
 * (above); the task exists so the firmware exposes a
 * `ps`-visible "button_poll" thread per FW-23 conventions.
 * Priority 5, stack 4096. The task body just calls
 * vTaskDelay in a loop; the esp_timer task does the real
 * work. */
static void button_poll_task(void *arg)
{
    (void)arg;
    while (g_initialized) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}
#else
/* Host stub: the mock esp_timer records this as a registered
 * callback, but the test never fires it. The test calls
 * button_poll_once() directly to advance the state machine.
 * The body is a no-op so the linker resolves the symbol. */
static void button_esp_timer_cb(void *arg)
{
    (void)arg;
}
#endif
