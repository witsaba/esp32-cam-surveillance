/* ws_reconnects.h — single declaration for the FW-14 owner hook.
 *
 * The `ws_reconnects_get()` symbol returns the WS reconnect
 * counter — FW-13 returns 0 (initial state); FW-14 owns the
 * real producer and replaces the implementation in
 * `ws_reconnects.c` without changing this header.
 *
 * Lives in a separate header (NOT ws.h) so FW-14 can swap the
 * impl without touching ws.h's other consumers (per design smell
 * #6 resolution). One consumer-facing header keeps the FW-14
 * refactor scoped.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return the current WS reconnect count. FW-13 returns 0 (the
 * real counter producer is owned by FW-14; charter L1201 +
 * design #3756 §1).
 *
 * Safe to call from any context; on host + device this is a
 * read of a module-static uint32_t (no locking needed for the
 * single-owner FW-13 stub; FW-14 will add atomic-safe read
 * semantics when the real producer lands). */
uint32_t ws_reconnects_get(void);

#ifdef __cplusplus
}
#endif