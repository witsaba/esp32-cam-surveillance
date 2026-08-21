/* boot.h — public API for the FR-1 boot orchestrator (FW-03).
 *
 * Call graph:
 *   app_main() ──▶ boot_run() ──▶ boot_decide_provisioning()
 *                              ├──▶ boot_run_provisioning() (FW-05 owns body)
 *                              └──▶ boot_run_normal()       (wifi/cam/ws/super)
 *
 * Stubs: the 7 stub interfaces below are call-sites only at FW-03
 * time. Each one is `// FW-NN: real impl lands in <milestone>`
 * inside the stub TU. They each return `ESP_OK` from a stub body;
 * host tests override their return values via `mock_init_returns`.
 *
 * Boot-button signal: weak symbol. Production builds link against
 * `boot_button_stub.c` (returns `false`). FW-07 replaces the strong
 * symbol. Host tests macro-override the symbol via
 * `mock_boot_link.h` so the `mocks` component can prime the return
 * value.
 */
#pragma once

#include <stdbool.h>

#include "boot_status.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The dispatcher. Returns a `boot_status_t`; `.step == BOOT_STEP_RETURN`
 * and `.ret == ESP_OK` on green path. On failure, `.step` names the
 * failing step and `.ret` carries the underlying error. The caller
 * (`app_main`) MUST discard the return — `app_main` returns void. */
boot_status_t boot_run(void);

/* Provisioning branch. Holds the device in the softAP + HTTP server
 * state (FW-05 owns the body). At FW-03 time, this function logs
 * "boot: provisioning branch entered" and returns. MUST NOT start
 * supervision tasks. */
boot_status_t boot_run_provisioning(const config_t *cfg);

/* Normal branch. Runs wifi_init → camera_init → ws_init → 4×
 * _task_start in the FR-1 order. Each step's return value is
 * checked; on non-OK, emits `ESP_LOGE("boot", "step=%s err=%s", …)`
 * with the failing step name and returns immediately. */
boot_status_t boot_run_normal(const config_t *cfg);

/* Pure decision function. Returns `true` when the device must enter
 * the provisioning branch: either `button_pressed` is asserted OR
 * `cfg->wifi.ssid` is the empty string. MUST NOT consult any global
 * state; both inputs come from arguments. Marked `noinline` on the
 * definition (see boot.c) so the FW-03.4 stub-and-flip bite-proof
 * cannot be defeated by the optimizer. */
bool boot_decide_provisioning(const config_t *cfg, bool button_pressed);

/* ---------- stub interfaces (FW-03 call-sites, real impls land later) ---------- */

/* Stub. Returns the value primed by `mock_init_returns_get(BOOT_STEP_WIFI_INIT)`
 * on host, or `ESP_OK` on device. Production impl lands in FW-08. */
esp_err_t wifi_init(const config_t *cfg);

/* Stub. Returns the value primed by `mock_init_returns_get(BOOT_STEP_CAMERA_INIT)`
 * on host, or `ESP_OK` on device. Production impl lands in FW-10. */
esp_err_t camera_init(const config_t *cfg);

/* Stub. Returns the value primed by `mock_init_returns_get(BOOT_STEP_WS_INIT)`
 * on host, or `ESP_OK` on device. Production impl lands in FW-13. */
esp_err_t ws_init(const config_t *cfg);

/* Supervision-task stubs. Each creates a no-op FreeRTOS task named
 * "stub_<role>" (observable at FW-23 via `ps`). Returns the value
 * primed by `mock_init_returns_get(BOOT_STEP_SUPERVISION_<ROLE>)`
 * on host, or `ESP_OK` on device. Real impls land in FW-16/11/15/18. */
esp_err_t health_task_start(void);
esp_err_t capture_task_start(void);
esp_err_t stream_task_start(void);
esp_err_t control_task_start(void);

/* Boot-button signal. Weak default returns `false`. FW-07's component
 * provides a strong symbol that consults the GPIO + press-duration
 * measurement. Host tests macro-override the symbol via
 * `mock_boot_link.h` so `mock_boot_button_set(bool)` primes the value. */
bool boot_button_pressed_at_boot(void);

#ifdef __cplusplus
}
#endif