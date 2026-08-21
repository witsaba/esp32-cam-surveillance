/* boot.c — FR-1 boot orchestrator (FW-03).
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
#include "config.h"

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
#include "mock_boot_link.h"
#include "mock_nvs_flash_link.h"
#else
#include "nvs_flash.h"
#endif

static const char *TAG = "boot";

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
    if (button_pressed) return true;
    return cfg->wifi.ssid[0] == '\0';
}

/* Provisioning branch. FW-05 owns the body (softAP + HTTP server).
 * At FW-03 time we only log and return. MUST NOT start supervision
 * tasks. */
boot_status_t boot_run_provisioning(const config_t *cfg)
{
    (void)cfg;
    ESP_LOGI(TAG, "boot: provisioning branch entered (FW-05 owns the body)");
    boot_status_t s = { .ret = ESP_OK, .step = BOOT_STEP_RETURN };
    return s;
}

/* Normal branch — the FR-1 sequence. Each step's return value is
 * wrapped in a `boot_status_t` tagged with the failing `boot_step_t`,
 * and on non-OK the orchestrator emits an `ESP_LOGE("boot",
 * "step=%s err=%s", boot_step_str(step), esp_err_to_name(ret))` line
 * so the failing step is human-readable + greppable. */
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
    BOOT_CHECK_STEP(BOOT_STEP_CAMERA_INIT,          camera_init(cfg));
    BOOT_CHECK_STEP(BOOT_STEP_WS_INIT,              ws_init(cfg));
    BOOT_CHECK_STEP(BOOT_STEP_SUPERVISION_HEALTH,   health_task_start());
    BOOT_CHECK_STEP(BOOT_STEP_SUPERVISION_CAPTURE,  capture_task_start());
    BOOT_CHECK_STEP(BOOT_STEP_SUPERVISION_STREAM,   stream_task_start());
    BOOT_CHECK_STEP(BOOT_STEP_SUPERVISION_CONTROL,  control_task_start());

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
        [BOOT_STEP_RETURN]                  = "return",
    };
    if ((unsigned)step >= BOOT_STEP_COUNT) return "unknown";
    return names[step];
}