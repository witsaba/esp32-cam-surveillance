/* boot.c — FR-1 boot orchestrator (FW-03) + FW-07.3 runtime cb
 * dispatch (Phase D).
 *
 * Owns `boot_run()`, `boot_run_provisioning()`, `boot_run_normal()`,
 * and `boot_decide_provisioning()`. The dispatcher (`boot_run`)
 * runs the FR-1 step 2 decision (empty-SSID OR button-pressed →
 * provisioning branch). On the normal branch it runs:
 *
 *     wifi_init → camera_init → ws_init →
 *     health_task_start → capture_task_start →
 *     stream_task_start → control_task_start
 *
 * After all supervision tasks start, the orchestrator registers
 * `boot_factory_reset_and_restart` as the button driver
 * RUNTIME-phase long-press callback (FW-07.3). The callback is
 * NOT registered on the provisioning branch — provisioning mode
 * already drives the softAP + HTTP server, and a runtime reset
 * on the provisioning branch would re-enter the boot orchestrator
 * without ever touching the dispatcher's decision logic.
 *
 * On any non-OK return from a stub, the orchestrator wraps the
 * step in a tagged `boot_status_t { .step = failing step,
 * .ret = underlying err }` and returns immediately. The FW-03.2
 * fail-loud commit adds the named `ESP_LOGE` log line; this
 * commit (FW-03.1) only emits the typed status.
 *
 * On host, `mock_boot_link.h` redirects `boot_button_pressed_at_boot`
 * to `mock_boot_button_pressed_at_boot_impl` so tests can prime the
 * signal via `mock_boot_button_set(bool)`. On device, the weak
 * stub in `boot_button_stub.c` returns `false`.
 */
#include "boot.h"
#include "boot_status.h"
#include "button.h"
#include "config.h"
#include "health.h"
#include "softap.h"
#include "stream_cmd.h"

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
#include "mock_boot_link.h"
#include "mock_nvs_flash_link.h"
#include "mock_esp_system_link.h"
#include "mock_config_link.h"
#else
#include "esp_system.h"
#include "nvs_flash.h"
#endif

static const char *TAG = "boot";

/* FW-07.3 — runtime factory-reset callback. Invoked by the
 * button driver when the user holds the button for ≥
 * RUNTIME_LONGPRESS_MS during the RUNTIME phase (FW-07.3
 * contract; PRD § FR-7 L236). The callback wipes the NVS
 * `config` namespace via config_factory_reset() and reboots via
 * esp_restart(); if the wipe fails the callback logs the
 * underlying error and returns WITHOUT rebooting — the user
 * can retry by holding the button again.
 *
 * Registered by `boot_run_normal()` (NOT `boot_run_provisioning()`)
 * because the provisioning branch already drives the softAP +
 * HTTP server and a runtime reset there would re-enter the
 * orchestrator without passing through the decision logic.
 *
 * On host, the production call sites `config_factory_reset()`
 * and `esp_restart()` are macro-redirected to the FW-07.3 +
 * FW-05 mocks via `mock_config_link.h` and
 * `mock_esp_system_link.h` — see those headers for details.
 */
static void boot_factory_reset_and_restart(void)
{
    esp_err_t err = config_factory_reset();
    if (err != ESP_OK) {
        /* Do NOT restart if the wipe failed — the user can
         * retry by holding the button again. The error is
         * logged for greppability + post-mortem
         * investigation. */
        ESP_LOGE(TAG, "config_factory_reset failed: %s",
                 esp_err_to_name(err));
        return;
    }
    esp_restart();
}

/* Step-2 decision: empty SSID OR button-pressed → provisioning.
 *
 * Marked `noinline` on the definition so the FW-03.4 stub-and-flip
 * bite-proof cannot be defeated by the compiler inlining the body
 * (the macro-redirect via `mock_boot_link.h` would otherwise be
 * bypassed on the inlined copy). The attribute is harmless on
 * device. */
__attribute__((noinline))
bool boot_decide_provisioning(const config_t *cfg, bool button_pressed)
{
#ifdef BOOT_TEST_STUB_FLIP_DECISION
    /* FW-03.4 bite-proof: the function alternates between true /
     * false on each call so the determinism invariant fails. The
     * counter is a process-local static so its state survives
     * across the two calls in `test_boot_stability_guard.c`. */
    static int flip_state = 0;
    (void)cfg;
    (void)button_pressed;
    flip_state ^= 1;
    return flip_state != 0;
#else
    if (button_pressed) return true;
    return cfg->wifi.ssid[0] == '\0';
#endif
}

/* Provisioning branch. FW-05 owns the body (softAP + HTTP server).
 * MUST NOT start supervision tasks. MUST NOT register the
 * runtime factory-reset cb (the device is already in provisioning
 * mode; a runtime reset there would re-enter the orchestrator
 * and bypass the decision step). */
boot_status_t boot_run_provisioning(const config_t *cfg)
{
    ESP_LOGI(TAG, "fw: provisioning branch entered (softAP body)");
    return softap_run_provisioning(cfg);
}

/* Normal branch — the FR-1 sequence. Each step's return value is
 * wrapped in a `boot_status_t` tagged with the failing `boot_step_t`,
 * and on non-OK the orchestrator emits an `ESP_LOGE("boot",
 * "step=%s err=%s", boot_step_str(step), esp_err_to_name(ret))` line
 * so the failing step is human-readable + greppable.
 *
 * FW-07.3 — after the supervision tasks start (system is
 * running), initialize the button driver and register
 * `boot_factory_reset_and_restart` as the runtime long-press cb.
 * The registration is the LAST step — by then the system is
 * fully initialized and the button driver is the only remaining
 * subsystem to bring up before handing control back to the IDF
 * event loop. */
boot_status_t boot_run_normal(const config_t *cfg)
{
    boot_status_t s = { .ret = ESP_OK, .step = BOOT_STEP_RETURN };

#define BOOT_CHECK_STEP(STEP_ENUM, CALL_EXPR) do {                   \
        esp_err_t _r = (CALL_EXPR);                                   \
        if (_r != ESP_OK) {                                            \
            s.ret = _r;                                                \
            s.step = (STEP_ENUM);                                      \
            ESP_LOGE(TAG, "step=%s err=%s",                           \
                     boot_step_str(s.step), esp_err_to_name(s.ret));   \
            return s;                                                 \
        }                                                              \
    } while (0)

    BOOT_CHECK_STEP(BOOT_STEP_WIFI_INIT,            wifi_init(cfg));

    /* FW-05.5 — install the always-on /whoami listener on the STA
     * interface. Subscribes IP_EVENT_STA_GOT_IP + DISCONNECTED but
     * does NOT start the httpd here (waits for the first IP-up).
     * Non-fatal on failure: log + continue so a subscription error
     * doesn't brick the device (the softAP still serves /whoami
     * during provisioning, so we don't lose the operator surface
     * entirely). */
    {
        esp_err_t r_listener = softap_sta_listener_install();
        if (r_listener != ESP_OK) {
            ESP_LOGE(TAG, "softap_sta_listener_install failed: %s "
                          "(continuing — softAP /whoami still works)",
                          esp_err_to_name(r_listener));
        }
    }

    /* FW-07.3 — initialize the button driver BEFORE camera_init.
     *
     * HARDWARE CONSTRAINT (AI-Thinker ESP32-CAM): GPIO 0 is shared
     * between the boot button and the camera XCLK. The camera's
     * esp_camera_init claims GPIO 0 as a 10 MHz clock OUTPUT; the
     * button's gpio_config (input + internal pull-up) would then
     * reconfigure the same pin and destroy the sensor's master
     * clock — every subsequent esp_camera_fb_get times out
     * (reproduced 2026-08-24 with staged fb_get probes: frames OK
     * through ws_init, NULL immediately after button_init when it
     * ran last).
     *
     * Ordering rule: the button claims the pin FIRST; the camera
     * claims it LAST. Runtime long-press detection still works:
     * pressing the button shorts GPIO 0 to GND, which dominates
     * the XCLK square wave, so the polled low-level duration is
     * unaffected. */
    (void)cfg;
    esp_err_t bi_r = button_init();
    if (bi_r != ESP_OK) {
        s.ret = bi_r;
        s.step = BOOT_STEP_BUTTON_INIT;
        ESP_LOGE(TAG, "step=%s err=%s",
                 boot_step_str(s.step), esp_err_to_name(s.ret));
        return s;
    }
    esp_err_t cb_r = button_on_runtime_longpress_set(
        boot_factory_reset_and_restart);
    if (cb_r != ESP_OK) {
        s.ret = cb_r;
        s.step = BOOT_STEP_BUTTON_INIT;
        ESP_LOGE(TAG, "step=%s err=%s",
                 boot_step_str(s.step), esp_err_to_name(s.ret));
        return s;
    }

    BOOT_CHECK_STEP(BOOT_STEP_CAMERA_INIT,          camera_init(cfg));
    BOOT_CHECK_STEP(BOOT_STEP_WS_INIT,              ws_init(cfg));
    BOOT_CHECK_STEP(BOOT_STEP_SUPERVISION_HEALTH,   health_task_start());
    BOOT_CHECK_STEP(BOOT_STEP_SUPERVISION_CAPTURE,  capture_task_start());
    BOOT_CHECK_STEP(BOOT_STEP_SUPERVISION_STREAM,   stream_task_start());
    BOOT_CHECK_STEP(BOOT_STEP_SUPERVISION_CONTROL,  control_task_start());

    /* FW-19 — stream command handler registration. Plain call
     * AFTER the CONTROL step: registration is an infallible array
     * store, so it adds NO boot step (boot-order assert stays
     * untouched) and cannot fail the boot sequence. */
    stream_cmd_register();

#undef BOOT_CHECK_STEP

    return s;
}

boot_status_t boot_run(void)
{
    /* FR-1 step 1: NVS init + config load. */
    esp_err_t nvs_r = nvs_flash_init();
    if (nvs_r != ESP_OK) {
        boot_status_t s = { .ret = nvs_r, .step = BOOT_STEP_NVS_INIT };
        return s;
    }

    config_t cfg;
    bool dirty = false;
    config_status_t cfg_r = config_load(&cfg, &dirty);
    if (cfg_r != CONFIG_OK) {
        boot_status_t s = { .ret = (esp_err_t)cfg_r, .step = BOOT_STEP_CONFIG_LOAD };
        return s;
    }

    /* FW-16.2 — surface the persisted soft-recovery reason (if any)
     * IMMEDIATELY after config_load succeeds, BEFORE the
     * provisioning decision, so the log line surfaces in BOTH
     * branches. Best-effort: never alters boot flow. */
    health_log_last_recovery_reason();

    /* FR-1 step 2: provisioning decision. */
    bool button_pressed = boot_button_pressed_at_boot();
    bool provisioning = boot_decide_provisioning(&cfg, button_pressed);

    boot_status_t decision_s = {
        .ret = ESP_OK,
        .step = BOOT_STEP_PROVISIONING_DECISION,
    };
    (void)decision_s;  /* tag the decision step (no fail path here) */

    if (provisioning) {
        return boot_run_provisioning(&cfg);
    }

    /* FR-1 step 3–8: normal branch. */
    boot_status_t s = boot_run_normal(&cfg);
    if (s.ret == ESP_OK) {
        ESP_LOGI(TAG, "fw: boot_run ret=ok step=return");
    }
    s.step = (s.ret == ESP_OK) ? BOOT_STEP_RETURN : s.step;
    return s;
}

/* `boot_step_str` lives in boot.c (not boot_status.h) so the static
 * names table is file-local and the host build does not need any
 * extra translation unit. Returns "unknown" for out-of-range input. */
const char *boot_step_str(boot_step_t step)
{
    static const char *names[BOOT_STEP_COUNT] = {
        [BOOT_STEP_NVS_INIT]                = "nvs_init",
        [BOOT_STEP_CONFIG_LOAD]             = "config_load",
        [BOOT_STEP_PROVISIONING_DECISION]   = "provisioning_decision",
        [BOOT_STEP_WIFI_INIT]               = "wifi_init",
        [BOOT_STEP_CAMERA_INIT]             = "camera_init",
        [BOOT_STEP_WS_INIT]                 = "ws_init",
        [BOOT_STEP_SUPERVISION_HEALTH]      = "supervision_health",
        [BOOT_STEP_SUPERVISION_CAPTURE]     = "supervision_capture",
        [BOOT_STEP_SUPERVISION_STREAM]      = "supervision_stream",
        [BOOT_STEP_SUPERVISION_CONTROL]     = "supervision_control",
        [BOOT_STEP_SOFTAP_START]            = "softap_start",
        [BOOT_STEP_BUTTON_INIT]             = "button_init",
        [BOOT_STEP_RETURN]                  = "return",
    };
    if ((unsigned)step >= BOOT_STEP_COUNT) return "unknown";
    return names[step];
}