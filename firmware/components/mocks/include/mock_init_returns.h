/* mock_init_returns.h — per-step return-value slots for the FW-03
 * ordering and bite-proof tests. Indexed by `boot_step_t`; default
 * ESP_OK. Each stub in `stub_inits.c` / `stub_supervision.c`
 * consults its slot via `mock_init_returns_get()` BEFORE returning
 * the value.
 *
 * Lives under `firmware/components/mocks/` (parallel to
 * `mock_nvs_flash_link.h`) so the host-test build can prime
 * non-OK returns without a per-init mock file. Device builds
 * never see this header — the stubs default to ESP_OK.
 */
#pragma once

#include "boot_status.h"

#ifdef __cplusplus
extern "C" {
#endif

void mock_init_returns_set(boot_step_t step, esp_err_t ret);
esp_err_t mock_init_returns_get(boot_step_t step);
void mock_init_returns_reset(void);

#ifdef __cplusplus
}
#endif