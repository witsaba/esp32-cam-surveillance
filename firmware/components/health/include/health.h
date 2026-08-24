/* health.h — FW-16 soft-recovery health-task surface.
 *
 * Provides the STRONG `health_task_start()` symbol (FW-15 stream
 * precedent): components/boot/boot.c calls it in the FR-1 sequence;
 * the former stub body in boot/stub_supervision.c was DELETED when
 * this component landed. `make test-stub` guards against any
 * duplicate-definition regression.
 *
 * Host contract (MUST match the deleted stub verbatim — asserted by
 * tests/test_boot/test_boot_order.c:84-96 `[health, capture, stream,
 * control]`): on UNITY_HOST_BUILD the call honours
 * `mock_init_returns_get(BOOT_STEP_SUPERVISION_HEALTH)` first (a
 * forced non-OK short-circuits WITHOUT recording), then records the
 * role via `mock_supervision_record("health")`.
 */
#ifndef HEALTH_H
#define HEALTH_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the health task (FR-1 supervision step 1). Emits the
 * greppable startup line
 *   `fw: health_task_start fails=<n> window_min=<n>`
 * at INFO under TAG `health` (device smoke asserts this literal).
 * Returns ESP_OK once the task is spawned; ESP_FAIL if xTaskCreate
 * fails on device; a forced mock error on host. */
esp_err_t health_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_H */
