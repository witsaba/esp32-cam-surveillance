/* mock_config.h — host-side mock for the `config` module's
 * factory-reset API (FW-07.3).
 *
 * FW-07.3 contract: when the runtime long-press cb fires, the
 * production wiring in `boot.c::boot_factory_reset_and_restart`
 * calls `config_factory_reset()` (FW-02 API) then
 * `esp_restart()` (FW-05 mock-redirected API). On host, the
 * `config_factory_reset()` call is redirected to
 * `mock_config_factory_reset()` via the macro override in
 * `mock_config_link.h`. The mock records the call count and
 * returns the value primed by `mock_config_factory_reset_set_return`
 * (default CONFIG_OK).
 *
 * Why a dedicated mock rather than the existing `mock_nvs_*`:
 * the production `config_factory_reset()` body (in `config.c`)
 * goes through the nvs mock and uses the in-memory mock_nvs
 * state. FW-07.3's contract is on the `config_factory_reset`
 * call count + return value — independent of the nvs state
 * evolution. A dedicated callback-shaped mock lets the test
 * assert the count/return without depending on the nvs mock's
 * open/close/erase_all path. On device, the macro is gated by
 * `MOCK_CONFIG_USE_REAL` and is not defined, so the real
 * `config_factory_reset()` is called.
 */
#pragma once

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Prime the return value of `mock_config_factory_reset()`. Default
 * CONFIG_OK after `mock_config_reset()`. Mirrors the
 * `_set_return` pattern used by `mock_esp_timer_create_set_return`
 * et al. */
void mock_config_factory_reset_set_return(config_status_t ret);

/* Recorded number of invocations since the last
 * `mock_config_reset()`. The test asserts `== 1` after the
 * runtime cb fires. */
int mock_config_factory_reset_call_count(void);

/* Reset the call counter and return-value slot to defaults.
 * Call from each test's setUp. */
void mock_config_reset(void);

/* Mock implementation of the FW-02 `config_factory_reset()` API.
 * Called via the `mock_config_link.h` macro override on host. */
config_status_t mock_config_factory_reset(void);

#ifdef __cplusplus
}
#endif
