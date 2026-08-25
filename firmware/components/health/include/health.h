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

#include <stdbool.h>
#include <stddef.h>

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

/* Persist the forensic recovery reason (R-FW16-1.2, AD4). Writes
 * HEALTH_RECOVERY_REASON_STR ("soft_recovery_threshold") to NVS
 * key "last_recovery_reason" in the DEDICATED namespace "recovery"
 * (survives config_factory_reset, which erases only ns "config").
 * Single attempt in config.c style (open READWRITE → set_str →
 * commit → close); a failure logs ESP_LOGE but is NON-FATAL — the
 * recovery sequence must never wedge the device. Called strictly
 * BEFORE esp_restart() on the trigger path. */
void health_persist_last_recovery_reason(void);

/* Next-boot surfacing (R-FW16-1.2, AD5). Best-effort READONLY read
 * of the stored reason; on a hit logs
 *   `fw: last_recovery_reason: <reason>`
 * at INFO under TAG `health`. NOT_FOUND → silent; any other error →
 * warn. NEVER alters boot flow or boot_status — safe to call right
 * after config_load succeeds. */
void health_log_last_recovery_reason(void);

/* ---- FW-16.3 bite-proof surface (Pass 13 builds ONLY) ----
 * Compiled exclusively under -DHEALTH_TEST_STUB_COUNT_WHILE_HEALTHY=1
 * (applied to BOTH this component's sources AND test_health_guard.c).
 * The tick models a future always-sweeping miscounting
 * implementation: each call records ONE phantom failure per tick —
 * no wifi event, no episode latch. In a correct event-driven-only
 * implementation nothing else advances the counter during healthy
 * streaming, so the guard test's count==0 / !should_recover
 * assertions fail with the literal "healthy-stream" — which is what
 * runner Pass 13 greps for. Absent from production builds. */
#ifdef HEALTH_TEST_STUB_COUNT_WHILE_HEALTHY
void health_green_path_tick_for_guard(void);
size_t health_guard_failure_count(void);
bool health_guard_should_recover(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_H */
