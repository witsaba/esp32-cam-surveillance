/* stream_cmd.h — FW-19 `stream` command handler (public surface).
 *
 * Registers against the FW-18 control registry and owns the whole
 * wire contract for the stream command:
 *
 *   START  {"cmd":"stream","on":true[,"fps":N]}
 *       → {"type":"stream_ok","on":true,"fps":<applied>}
 *         (ack sent BEFORE the capture gate opens — design D3 pin)
 *   STOP   {"cmd":"stream","on":false}
 *       → {"type":"stream_ok","on":false,"fps":<applied>}
 *         (gate cleared FIRST, then ack; idempotent, viewerless-ok)
 *   errors → {"type":"error","reason":"bad_field"|"no_viewer",
 *             "id":<orig>} via control_error_build (rulings 6/7).
 *
 * `fps` semantics (ruling 1 + ruling 6): absent ⇒ CONFIG default;
 * out-of-RANGE integers clamp silently to [MIN..MAX]; wrong-TYPE
 * values (string / float / null) refuse bad_field. START requires
 * an active viewer (ws_server_viewer_active); STOP never does.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plug-in seam called once from boot.c after the CONTROL step.
 * Plain call — registration is an infallible array store, so it
 * adds NO boot step (boot-order assert untouched). */
void stream_cmd_register(void);

/* Ack builder (control_error_build precedent): renders
 * {"type":"stream_ok","on":<bool>,"fps":<u32>} into `out`.
 * Returns bytes written, 0 on overflow/invalid args — the caller
 * skips the send on the 0-sentinel. Exposed for byte-exact unit
 * coverage of the ruling-1 envelope shape. */
size_t stream_ok_build(bool on, uint32_t fps, char *out,
                       size_t out_len);

#ifdef __cplusplus
}
#endif
