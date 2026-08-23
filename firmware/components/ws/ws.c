/* ws.c — WS client init skeleton (FW-13, T-13-C GREEN-only).
 *
 * T-13-C scope: provide the module-static storage + the host-only
 * mock_init_returns_get short-circuit that the FW-03.2 bite-proof
 * relies on, but DO NOT export a strong `ws_init` symbol yet —
 * `firmware/components/boot/stub_inits.c::ws_init` is still the
 * active implementation (the host runner links both files; the
 * linker would fail on duplicate-symbol if we defined `ws_init`
 * here today).
 *
 * T-13-J replaces `stub_inits.c::ws_init` with the production
 * body from `ws_init_impl()` defined here. Until then:
 *
 *   - `ws_init_impl()` exists in this TU and is non-static so the
 *     linker keeps it; it's NOT referenced by any caller. The
 *     T-13-J integration commit renames it back to `ws_init` (the
 *     public symbol consumed by boot.c:153) and deletes the
 *     stub_inits.c body in the same atomic commit.
 *
 *   - The host runner compiles BOTH stub_inits.c and ws.c. The
 *     linker keeps the first-defined `ws_init` (from
 *     stub_inits.c, included earlier in all_sources). The
 *     `ws_init_impl` body in this TU is reachable via
 *     `ws_init_shim_dummy()` below, which the runner never
 *     actually invokes.
 */
#include "ws.h"

#include <string.h>

#include "esp_log.h"

#ifdef UNITY_HOST_BUILD
#include "mock_init_returns.h"
#include "boot_status.h"
#endif

static const char *TAG = "ws";

/* Module-static config pointer. The real init body (T-13-D
 * GREEN) builds an esp_websocket_client_config_t from this +
 * the Kconfig defaults and passes it to esp_websocket_client
 * _init(). Today (T-13-C) we just retain the pointer so a future
 * refactor has a stable handle. */
static const config_t *s_ws_cfg = NULL;

/* Module-static helper used by both the public `ws_init` (post
 * T-13-J) and the T-13-C init shim below. Honours the
 * mock_init_returns_get short-circuit so the FW-03.2 bite-proof
 * trips the same way it did under stub_inits.c::ws_init. */
static esp_err_t ws_init_short_circuit(const config_t *cfg)
{
#ifdef UNITY_HOST_BUILD
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_WS_INIT);
    if (forced != ESP_OK) return forced;
#endif
    s_ws_cfg = cfg;
    ESP_LOGI(TAG, "ws_init: skeleton (T-13-C) — real impl lands in T-13-D");
    return ESP_OK;
}

/* Public symbol consumed by boot.c:153 (T-13-J replaces the
 * stub_inits.c body with this). Non-static so the linker can
 * resolve it once the stub is deleted. NOT called by anyone
 * today — the host runner's stub_inits.c::ws_init wins first
 * inclusion; on device boot.c:153 also resolves to
 * stub_inits.c::ws_init until T-13-J. */
esp_err_t ws_init_impl(const config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    return ws_init_short_circuit(cfg);
}

/* Touch `ws_init_impl` so the linker keeps it in the symbol
 * table even though no caller invokes it today. Removed when
 * T-13-J makes `ws_init_impl` the public `ws_init`. */
void ws_init_shim_dummy(void)
{
    (void)ws_init_impl;
}