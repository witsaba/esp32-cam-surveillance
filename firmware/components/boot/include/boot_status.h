/* boot_status.h — typed-error contract for the boot orchestrator.
 *
 * Every init step in the FR-1 sequence maps to a `boot_step_t`
 * value. On failure, the orchestrator returns a `boot_status_t`
 * with `.step` naming the failing step and `.ret` carrying the
 * underlying esp_err_t. The error log line uses
 * `boot_step_str(step)` so the failing step is human-readable and
 * greppable for the FW-03.2 bite-proof.
 *
 * `BOOT_STEP_RETURN` is the sentinel for a successful run (no
 * step is "currently failing"). `BOOT_STEP_COUNT` lets test code
 * size arrays indexed by step.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOOT_STEP_NVS_INIT                 = 0,  /* nvs_flash_init()             */
    BOOT_STEP_CONFIG_LOAD              = 1,  /* config_load()                */
    BOOT_STEP_PROVISIONING_DECISION    = 2,  /* boot_decide_provisioning()   */
    BOOT_STEP_WIFI_INIT                = 3,  /* wifi_init() stub             */
    BOOT_STEP_CAMERA_INIT              = 4,  /* camera_init() stub           */
    BOOT_STEP_WS_INIT                  = 5,  /* ws_init() stub               */
    BOOT_STEP_SUPERVISION_HEALTH       = 6,  /* health_task_start()          */
    BOOT_STEP_SUPERVISION_CAPTURE      = 7,  /* capture_task_start()         */
    BOOT_STEP_SUPERVISION_STREAM       = 8,  /* stream_task_start()          */
    BOOT_STEP_SUPERVISION_CONTROL      = 9,  /* control_task_start()         */
    BOOT_STEP_SOFTAP_START             = 11, /* softAP bring-up + httpd_start (FW-05) */
    BOOT_STEP_RETURN                   = 12, /* orchestrator returned        */
    BOOT_STEP_COUNT                    = 13
} boot_step_t;

/* Tagged-error return. `.step` is the FR-1 step where the run halted
 * (or `BOOT_STEP_RETURN` on green path). `.ret` is the underlying
 * `esp_err_t` — `ESP_OK` only when `.step == BOOT_STEP_RETURN`. */
typedef struct {
    esp_err_t ret;
    boot_step_t step;
} boot_status_t;

/* Human-readable name for a step. Returns "unknown" for out-of-range
 * input. The returned pointer has static storage; do NOT free. */
const char *boot_step_str(boot_step_t step);

#ifdef __cplusplus
}
#endif