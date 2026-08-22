/* button.c — FW-07 boot-button driver (R-03 measurement half + R-24
 * runtime factory reset). Phase D + E implementation: FW-07.2
 * boot-time long-press detection (latch + BOOT_TIME window +
 * strap-grace transient absorption) + FW-07.3 runtime
 * long-press callback dispatch + FW-07.4 debounce filter
 * (collapses contact-bounce jitter into one transition).
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
 *      the RUNTIME phase. `button_on_runtime_longpress_set()`
 *      stores the cb pointer; each poll inside the RUNTIME+PRESSED
 *      sub-state checks whether the press duration has crossed
 *      RUNTIME_LONGPRESS_MS and fires the cb (gated by a
 *      DEBOUNCE_MS*4 cooldown so the cb does not double-fire
 *      while the user keeps holding past the threshold). Production
 *      wires `config_factory_reset() + esp_restart()` into the
 *      callback (registered in `boot.c::boot_run_normal()`).
 *
 *   3. **Strap-pin tolerance** (FW-07.1 — Phase B, this file) —
 *      the GPIO-0 ROM bootloader transient (~100 ms LOW after
 *      reset) is absorbed by a STRAP_GRACE_MS window after
 *      `button_init()`. Polls before the window expires return
 *      without state change.
 *
 *   4. **Debounce filter** (FW-07.4 — Phase E, this file) —
 *      every GPIO edge (falling OR rising) that lands within
 *      DEBOUNCE_MS (default 20 ms) of the previously-accepted
 *      edge is treated as contact-bounce and ignored. The
 *      filter body is gated by
 *      `#ifdef BUTTON_TEST_STUB_DISABLE_DEBOUNCE` (mirrors the
 *      FW-06 LED pattern around esp_timer_create) so Pass 6
 *      of run_host_tests.py can exercise the bite-proof on
 *      jitter-induced phantom presses.
 *
 * State machine (Phase C + D — FW-07.1 + FW-07.2 + FW-07.3):
 *
 *   phase: STRAP_GRACE → BOOT_TIME → RUNTIME
 *   state: IDLE → PRESSED → IDLE
 *
 *   STRAP_GRACE: ignore GPIO reads.
 *   BOOT_TIME: measure boot-time press. Latch
 *     `g_boot_button_pressed_at_boot` when duration crosses
 *     BOOT_LONGPRESS_MS.
 *   RUNTIME: measure runtime press. Fire the registered cb when
 *     duration crosses RUNTIME_LONGPRESS_MS (Phase D dispatch).
 *
 *   IDLE + falling edge → PRESSED; g_press_start_us = now_us;
 *     g_runtime_cb_fired_at_us = 0 (fresh press: clear stale
 *     cooldown).
 *   PRESSED + rising edge:
 *     duration = now_us - g_press_start_us
 *     if duration ≤ TAP_MAX_US:  → IDLE (TAP — ignore)
 *     else if phase == BOOT_TIME + duration ≥ BOOT_LONGPRESS_MS:
 *                                  latch g_boot_button_pressed_at_boot
 *                                  → IDLE
 *     else if phase == RUNTIME + duration ≥ RUNTIME_LONGPRESS_MS:
 *                                  fire registered cb (cooldown
 *                                  gated) → IDLE
 *     else:                      → IDLE (qualified press)
 *   PRESSED + each poll (while held in BOOT_TIME): check duration;
 *     if ≥ BOOT_LONGPRESS_MS, latch `g_boot_button_pressed_at_boot`
 *     sticky-true (handles S6 — press continues past threshold
 *     without a rising edge inside BOOT_TIME).
 *   PRESSED + each poll (while held in RUNTIME, Phase D): check
 *     duration; if ≥ RUNTIME_LONGPRESS_MS and (cb cooldown
 *     expired OR cb never fired), invoke the registered cb and
 *     record the fire timestamp.
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

/* FW-07.3 — RUNTIME long-press threshold (microseconds).
 * Compared against `now_us - g_press_start_us` while the state
 * machine is in BUTTON_PHASE_RUNTIME and PRESSED. Crossing this
 * threshold fires the registered runtime cb exactly once
 * (cooldown DEBOUNCE_MS * 4 prevents double-fire while the user
 * keeps holding past the threshold). Default 10000 ms per PRD
 * § FR-7 L236. */
static inline int64_t button_runtime_longpress_us(void)
{
    return (int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_RUNTIME_LONGPRESS_MS * 1000LL;
}

/* FW-07.3 — DEBOUNCE_MS*4 cooldown (microseconds) suppresses
 * the runtime cb fire while the user keeps holding past
 * RUNTIME_LONGPRESS_MS. Without the cooldown, every poll after
 * the threshold would fire the cb again on each tick. Default
 * 20 ms * 4 = 80 ms — long enough to absorb the continuous-read
 * race (one extra poll at the boundary) yet short enough to be
 * transparent to a user who releases and re-presses. */
static inline int64_t button_runtime_cooldown_us(void)
{
    return (int64_t)CONFIG_FIRMWARE_BOOT_BUTTON_DEBOUNCE_MS * 4 * 1000LL;
}

/* FW-07.4 — debounce filter (per PRD § FR-7 L234 "the user-press
 * signal is filtered through a DEBOUNCE_MS debounce so contact-
 * bounce jitter does not generate spurious press events"). Returns
 * true iff a new edge at `now_us` is INSIDE the debounce window
 * from the previously-recorded edge (`last_edge_us`) and should be
 * ignored as a contact-bounce artifact.
 *
 * The filter is implemented AS the gate's else-branch (mirrors the
 * FW-06 `#ifdef LED_TEST_STUB_DISABLE_TIMER` pattern around
 * `led.c::esp_timer_create()`):
 *
 *   - When BUTTON_TEST_STUB_DISABLE_DEBOUNCE is NOT defined
 *     (production): the filter enforces the DEBOUNCE_MS window —
 *     any edge whose time-since-last-edge is < DEBOUNCE_MS is
 *     considered contact-bounce and ignored.
 *   - When BUTTON_TEST_STUB_DISABLE_DEBOUNCE IS defined (test
 *     bite-proof): the filter is short-circuited to "never
 *     bouncing" so every edge is accepted. The Pass-6 test
 *     (firmware/tools/run_host_tests.py) exercises a jitter
 *     pattern that the production filter would collapse into
 *     a single transition; with the filter disabled the
 *     jitter is propagated and the bite-proof trips with
 *     "debounce" in the output.
 *
 * The function is static-inline so the compiler can constant-fold
 * it under the stub flag (the (void)last_edge_us + (void)now_us
 * cast suppresses the unused-parameter warning). */
#ifdef BUTTON_TEST_STUB_DISABLE_DEBOUNCE
/* Stub violation: debounce disabled — all edges pass. */
static inline bool button_edge_is_bouncing(int64_t last_edge_us,
                                            int64_t now_us)
{
    (void)last_edge_us;
    (void)now_us;
    return false;
}
#else
static inline bool button_edge_is_bouncing(int64_t last_edge_us,
                                            int64_t now_us)
{
    /* Sentinel INT64_MIN means "no previous edge" — the very
     * first edge at button_init() time is unconditionally
     * accepted because `now_us - INT64_MIN` is a huge positive
     * number that dwarfs DEBOUNCE_MS. Mirrors the
     * `g_phase = BUTTON_PHASE_COUNT` sentinel idiom used for
     * "uninitialized phase". */
    if (last_edge_us == INT64_MIN) return false;
    return (now_us - last_edge_us) <
           (int64_t)(CONFIG_FIRMWARE_BOOT_BUTTON_DEBOUNCE_MS * 1000);
}
#endif

/* Phase / state storage — single-byte/8-byte volatile, atomic
 * write on Xtensa LX6. Matches FW-03 lock-free idiom in boot.c. */
static volatile button_phase_t g_phase = BUTTON_PHASE_COUNT;  /* sentinel: uninit */
static volatile button_state_t g_state = BUTTON_STATE_IDLE;

/* Press-window timestamps. g_press_start_us is set on the
 * falling edge (IDLE → PRESSED); g_press_end_us is the
 * now_us of the most recent rising edge (PRESSED → IDLE).
 * See g_last_edge_us declaration below for the debounce
 * filter (Phase E) anchor.
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
/* FW-07.4 — `g_last_edge_us` feeds the debounce filter. The
 * sentinel value INT64_MIN means "no previous accepted edge"
 * — the very first edge after button_init() is unconditionally
 * accepted (the filter would otherwise swallow it as a 0-ms
 * intra-DEBOUNCE_MS "edge"). Updated only on ACCEPTED edges
 * (the filter compares each new edge against the last
 * ACCEPTED edge, ignoring bouncing edges' timestamps). */
static volatile int64_t g_last_edge_us      = INT64_MIN;
static volatile bool    g_press_in_progress = false;

/* FW-07.3 — runtime-press-window start timestamp. The runtime
 * long-press check measures duration from
 * `g_runtime_press_start_us`, NOT from `g_press_start_us`. This
 * matters for the S6 scenario: a press that started in
 * STRAP_GRACE and continues through BOOT_TIME into RUNTIME has
 * `g_press_start_us = 0` (the original falling edge — used for
 * the boot-time latch), but the runtime cb should only count
 * the time elapsed after the state machine enters RUNTIME. If
 * we measured from `g_press_start_us`, a 10 s press at
 * BOOT_TIME (S6 scenario) would falsely fire the runtime cb on
 * the rising edge (duration = 10 s ≥ RUNTIME_LONGPRESS_MS).
 *
 * `g_runtime_press_start_us` is set in two places:
 *   1. On the BOOT_TIME → RUNTIME transition while PRESSED,
 *      so the runtime counter starts from the transition
 *      moment (S6 case: cb does NOT fire because the
 *      remaining RUNTIME duration is below the threshold).
 *   2. On a fresh falling edge in RUNTIME (when no press is
 *      in progress), so a fresh press in RUNTIME also has a
 *      clean timer start.
 *
 * Reset on `button_deinit()`. */
static volatile int64_t g_runtime_press_start_us = 0;

/* FW-07.3 — runtime-cb fire timestamp. `g_runtime_cb_fired_at_us
 * = 0` means "never fired in this button_init() lifetime". After
 * the registered cb fires, the timestamp is set to the now_us
 * value of the firing poll; subsequent polls within the
 * DEBOUNCE_MS*4 cooldown window check this timestamp and
 * suppress re-fire. Clearing it on rising edge lets a fresh
 * press fire a fresh cb. Reset on `button_deinit()`. */
static volatile int64_t g_runtime_cb_fired_at_us = 0;

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
    /* g_last_edge_us = INT64_MIN sentinel: "no previous
     * accepted edge" — the very first edge after button_init
     * is unconditionally accepted by the debounce filter. */
    g_last_edge_us    = INT64_MIN;

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
    g_last_edge_us   = INT64_MIN;
    g_strap_release_us = 0;
    g_boot_time_end_us = 0;
    g_press_in_progress = false;
    g_boot_button_pressed_at_boot = false;
    g_runtime_longpress_cb = NULL;
    g_runtime_cb_fired_at_us = 0;
    g_runtime_press_start_us = 0;
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
 *       - Else: qualified press — return to IDLE.
 *
 * Phase D (FW-07.3) additions (RUNTIME cb dispatch):
 *   - While state == PRESSED and phase == RUNTIME, check on
 *     each poll whether the press duration has crossed
 *     RUNTIME_LONGPRESS_MS; if so:
 *       1. Compute the cooldown window: skip re-fire if
 *          (now_us - g_runtime_cb_fired_at_us) < DEBOUNCE_MS*4.
 *       2. Otherwise fire the registered cb (NULL = no-op) and
 *          record the fire timestamp for the cooldown.
 *   - On the rising edge in RUNTIME, clear the cooldown
 *     timestamp (g_runtime_cb_fired_at_us = 0) so a fresh press
 *     fires a fresh cb. Without this clear, a stale timestamp
 *     would suppress the next cb if it lands inside the
 *     cooldown window.
 *   - If no cb is registered, the RUNTIME check is a no-op
 *     (the cb pointer is NULL — see g_runtime_longpress_cb).
 *     No log line, no crash. This is the "silent ignore"
 *     contract.
 *
 * Phase E (FW-07.4) additions (debounce filter):
 *   - Every new edge (falling OR rising, in any phase) is
 *     checked against `g_last_edge_us`. If the time delta
 *     is < DEBOUNCE_MS (default 20 ms), the edge is treated
 *     as contact-bounce and ignored:
 *       - Falling edge ignored → state stays IDLE, no press
 *         window opens.
 *       - Rising edge ignored → state stays PRESSED, the
 *         press window stays open.
 *   - When an edge is ignored, g_last_edge_us is NOT updated
 *     (subsequent edges compare against the LAST ACCEPTED
 *     edge until jitter settles).
 *   - The filter body is gated by
 *     `#ifdef BUTTON_TEST_STUB_DISABLE_DEBOUNCE` (mirrors the
 *     FW-06 `#ifdef LED_TEST_STUB_DISABLE_TIMER` pattern).
 *     Under stub, the filter returns false (every edge
 *     passes) — used by Pass 6 of run_host_tests.py to
 *     exercise the bite-proof on jitter-induced phantom
 *     presses.
 */
void button_poll_once(int64_t now_us)
{
    if (!g_initialized) return;

    /* Strap-grace window: still read the GPIO so we capture
     * a falling edge that started before the window ended.
     * The state-machine transitions are deferred until the
     * window expires; once it does, transition to BOOT_TIME
     * and process this same poll as the first BOOT_TIME poll.
     *
     * FW-07.4 — debounce filter also applies inside
     * STRAP_GRACE. Without this, a jitter pattern that begins
     * inside the strap-grace window could record a stale
     * `g_press_start_us` and then a fresh BOOT_TIME poll would
     * see a state that doesn't match the GPIO. The filter
     * collapses jitter the same way regardless of phase. */
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
         * start a fresh press measurement.
         *
         * FW-07.4 — debounce: a new edge that lands within
         * DEBOUNCE_MS of the previously-accepted edge is
         * contact-bounce and must be ignored. */
        if (sg_pressed_now && !g_press_in_progress) {
            if (!button_edge_is_bouncing(g_last_edge_us, now_us)) {
                g_press_start_us = now_us;
                g_press_in_progress = true;
                g_last_edge_us    = now_us;
            }
        } else if (!sg_pressed_now && g_press_in_progress) {
            if (!button_edge_is_bouncing(g_last_edge_us, now_us)) {
                /* Rising edge during strap grace: the user
                 * released before the window ended. Clear
                 * the in-progress flag so a later falling
                 * edge in BOOT_TIME starts fresh. */
                g_press_in_progress = false;
                g_press_start_us    = 0;
                g_last_edge_us      = now_us;
            }
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
     * branch.
     *
     * If a press is in flight at the transition (state ==
     * PRESSED), seed `g_runtime_press_start_us = now_us` so
     * the runtime duration counter starts from THIS moment
     * — NOT from the original falling edge (which lives in
     * `g_press_start_us` and is preserved for the boot-time
     * latch). This is the Phase D correction that fixes S6:
     * a 10 s press starting inside STRAP_GRACE has
     * `g_press_start_us = 0` (the original falling edge), and
     * the runtime cb should NOT fire when the rising edge
     * lands at t=10000 — because only 4.5 s of that press
     * happened DURING RUNTIME. */
    if (g_phase == BUTTON_PHASE_BOOT_TIME &&
        now_us >= g_boot_time_end_us) {
        g_phase = BUTTON_PHASE_RUNTIME;
        if (g_state == BUTTON_STATE_PRESSED) {
            g_runtime_press_start_us = now_us;
        }
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
             * inside BOOT_TIME), record it now. A fresh
             * press also resets the runtime-cb fire
             * timestamp so the next RUNTIME threshold
             * crossing fires a fresh cb (Phase D). If we
             * are in RUNTIME phase at the falling edge,
             * seed g_runtime_press_start_us = now_us so
             * the runtime duration counter starts fresh.
             *
             * FW-07.4 — debounce filter: a falling edge
             * that lands within DEBOUNCE_MS of the
             * previously-accepted edge is contact-bounce
             * and is ignored. The press window does NOT
             * open and g_last_edge_us is NOT updated —
             * subsequent edges compare against the last
             * ACCEPTED edge until jitter settles. */
            if (button_edge_is_bouncing(g_last_edge_us, now_us)) {
                break;
            }
            if (!g_press_in_progress) {
                g_press_start_us = now_us;
                g_press_in_progress = true;
                if (g_phase == BUTTON_PHASE_RUNTIME) {
                    g_runtime_press_start_us = now_us;
                }
            }
            g_runtime_cb_fired_at_us = 0;
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
        /* Phase D (FW-07.3): while still held in RUNTIME,
         * check whether the press duration has crossed
         * RUNTIME_LONGPRESS_MS; if so, fire the registered cb
         * exactly once (DEBOUNCE_MS*4 cooldown prevents
         * double-fire on subsequent polls while the user
         * keeps holding). The duration is measured from
         * `g_runtime_press_start_us` (set on RUNTIME entry
         * or fresh falling edge in RUNTIME), NOT from
         * `g_press_start_us` — see the comment on the
         * g_runtime_press_start_us declaration for the S6
         * rationale. */
        if (pressed_now && g_phase == BUTTON_PHASE_RUNTIME) {
            int64_t held_us = now_us - g_runtime_press_start_us;
            if (held_us >= button_runtime_longpress_us()) {
                /* Cooldown: skip re-fire if the cb fired
                 * recently (within DEBOUNCE_MS*4). The cb
                 * itself may take longer than the cooldown
                 * if it does heavy work (NVS erase + esp
                 * restart), so the next poll AFTER the cb
                 * returns will likely be inside the cooldown
                 * — that is the expected double-fire
                 * suppression path. */
                int64_t since_fire = now_us - g_runtime_cb_fired_at_us;
                if (g_runtime_longpress_cb != NULL &&
                    (g_runtime_cb_fired_at_us == 0 ||
                     since_fire >= button_runtime_cooldown_us())) {
                    g_runtime_longpress_cb();
                    g_runtime_cb_fired_at_us = now_us;
                }
            }
        }
        if (!pressed_now) {
            /* Rising edge: close the press window. Two
             * duration measures are used below:
             *   - `total_duration_us = now_us - g_press_start_us`
             *     for the boot-time press check (preserves
             *     Phase C semantics: count the duration from
             *     the original falling edge, even if it
             *     landed inside STRAP_GRACE).
             *   - `runtime_duration_us = now_us - g_runtime_press_start_us`
             *     for the runtime cb dispatch (Phase D
             *     correction: count the duration only while
             *     the state machine was in RUNTIME — see
             *     g_runtime_press_start_us rationale).
             *
             * FW-07.4 — debounce filter: a rising edge
             * that lands within DEBOUNCE_MS of the
             * previously-accepted edge is contact-bounce
             * and is ignored. The state stays PRESSED
             * (the press window does NOT close) and
             * g_last_edge_us is NOT updated. */
            if (button_edge_is_bouncing(g_last_edge_us, now_us)) {
                break;
            }
            int64_t total_duration_us = now_us - g_press_start_us;
            int64_t runtime_duration_us = now_us - g_runtime_press_start_us;
            if (total_duration_us > button_tap_max_us()) {
                /* Qualified press. If we're still in
                 * BOOT_TIME and the press crossed
                 * BOOT_LONGPRESS_MS, latch the boot-time
                 * signal. This catches the rising-edge
                 * boundary case where the duration equals
                 * BOOT_LONGPRESS_MS exactly at the moment
                 * of release. */
                if (g_phase == BUTTON_PHASE_BOOT_TIME &&
                    total_duration_us >= button_boot_longpress_us()) {
                    g_boot_button_pressed_at_boot = true;
                }
                /* Phase D rising-edge boundary: also fire
                 * the runtime cb if the runtime duration
                 * crossed RUNTIME_LONGPRESS_MS exactly at
                 * the rising-edge boundary (in case the
                 * in-POLL check missed it — e.g. the press
                 * reached exactly the threshold at the
                 * release moment). */
                if (g_phase == BUTTON_PHASE_RUNTIME &&
                    runtime_duration_us >= button_runtime_longpress_us()) {
                    int64_t since_fire = now_us - g_runtime_cb_fired_at_us;
                    if (g_runtime_longpress_cb != NULL &&
                        (g_runtime_cb_fired_at_us == 0 ||
                         since_fire >= button_runtime_cooldown_us())) {
                        g_runtime_longpress_cb();
                        g_runtime_cb_fired_at_us = now_us;
                    }
                }
            }
            g_state       = BUTTON_STATE_IDLE;
            g_press_in_progress = false;  /* close the press window */
            g_press_start_us = 0;
            /* Clear the runtime-state timestamps so the
             * NEXT press in RUNTIME can fire a fresh cb.
             * Without this clear, a stale fire-timestamp
             * could fall inside the cooldown window on
             * the next press and suppress the cb even
             * when the user had released between presses;
             * a stale runtime_press_start_us would put
             * the duration counter in a bogus state.
             * Both timestamps are reset, mirroring the
             * boot-time-press-in-progress reset. */
            g_runtime_press_start_us = 0;
            g_runtime_cb_fired_at_us = 0;
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
