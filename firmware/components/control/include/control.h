/* control.h — FW-18 inbound command dispatcher (public surface).
 *
 * Pure router core (control_route.c): strict-cJSON classification of
 * inbound TEXT frames against the six-command allow-list, plus the
 * unified error-envelope builder. Zero ws/httpd/mock dependencies —
 * the RX seam (ws_server.c) hands over frame copies; the task shell
 * (control.c) pops, processes, and emits replies via ws_sink_send_text.
 *
 * Wire contract (ruling #3966.1): every rejected or unhandled command
 * answers {"type":"error","reason":"<token>","id":<orig>} where the id
 * echoes the original value AND type losslessly (#3966.4) and is
 * omitted entirely when the body carries no usable scalar id (D9).
 * Tokens: bad_json | unknown | not_implemented (retires at FW-21).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed queue geometry (rulings #3966.6/#3966.8; stream.c precedent —
 * NO Kconfig tunables in scope). */
#define CONTROL_QUEUE_DEPTH        8
#define CONTROL_FRAME_MAX          512
#define CONTROL_ID_ECHO_MAX        64
#define CONTROL_RECEIVE_TIMEOUT_MS 1000

/* The six-command allow-list (milestone order). Handlers register at
 * FW-19/20/21 init without touching the validated parser TU. */
typedef enum {
    CONTROL_CMD_STREAM = 0,
    CONTROL_CMD_CONFIG,
    CONTROL_CMD_RESET_CAM,
    CONTROL_CMD_SLEEP,
    CONTROL_CMD_REBOOT,
    CONTROL_CMD_IDENTIFY,
    CONTROL_CMD_COUNT
} control_cmd_id_t;

/* Classification outcomes for one inbound frame. */
typedef enum {
    CONTROL_PARSED_DISPATCH = 0, /* allow-listed AND handler registered */
    CONTROL_NOT_IMPLEMENTED,     /* allow-listed, no handler (FW-18 state) */
    CONTROL_UNKNOWN_CMD,         /* valid JSON, no usable string cmd */
    CONTROL_BAD_JSON             /* body failed strict parse */
} control_parse_status_t;

/* Parse result. `id` is a BORROWED pointer: into *doc_out when the
 * doc parsed, or into a salvage-scanner temporary flagged by
 * id_temp_owned (freed exactly once by control_frame_process). */
typedef struct {
    control_parse_status_t status;
    control_cmd_id_t cmd_id; /* valid iff DISPATCH or NOT_IMPLEMENTED */
    bool has_id;             /* usable scalar id recovered */
    bool id_temp_owned;      /* id points at a salvage temp item */
    const cJSON *id;         /* lossless original value/type */
} control_parsed_t;

/* FW-19+ handler contract: receives VALIDATED bodies only (FW-18.4
 * guard). Returns esp_err_t; no wire reply obligation. */
typedef esp_err_t (*control_handler_fn)(const char *raw_body, size_t len,
                                        void *ctx);

/* ---------- pure router core ---------- */

/* Strict-classify one frame. On success with a parsed doc, *doc_out
 * carries it (caller frees). Never returns NULL doc with a status
 * other than BAD_JSON. */
control_parse_status_t control_parse(const char *frame, size_t len,
                                     cJSON **doc_out, control_parsed_t *out);

/* Registry lookup ONLY (D2): resolves the handler slot for an
 * allow-listed parse; NULL when none registered. */
control_handler_fn control_dispatch(const control_parsed_t *p);

/* Process one frame end-to-end: parse → route → compose the reply
 * envelope into `out`. Returns bytes written, or 0 when nothing must
 * be sent (handler dispatched, overflow sentinel, or invalid args).
 * Owns the parse doc + salvage-temp lifetime (free-once). */
size_t control_frame_process(const char *frame, size_t len,
                             char *out, size_t out_len);

/* Unified error-envelope builder (D6). Shape
 * {"type":"error","reason":"<token>"[,"id":<echo>]}. Numbers render
 * unquoted; strings pass a bounded escaper capped at
 * CONTROL_ID_ECHO_MAX (longer string ids omit the member entirely —
 * ruling #3966.8). Returns bytes written, 0 on overflow/invalid. */
size_t control_error_build(const char *reason_token, const cJSON *id,
                           char *out, size_t out_len);

/* Plug-in seam for FW-19/20/21. Registration is permanent for the
 * process lifetime (init-time only). */
void control_handler_register(control_cmd_id_t id, control_handler_fn fn);

/* ---------- bounded command ring (D3, capture_queue_t-shaped) ---- */

/* Depth-8 SPSC ring. NULL sync_mtx/sync_cv (stack-instantiated host
 * test queues) keep the ops lock-free and pure; the module-static
 * instance gets pthread (host) / FreeRTOS (device) hooks armed by
 * control_task_start BEFORE the task spawns. */
typedef struct {
    void *slots[CONTROL_QUEUE_DEPTH];
    int   head;
    int   tail;
    int   count;
    void *sync_mtx;
    void *sync_cv;
} control_queue_t;

/* Push a heap-owned frame copy. Returns true when enqueued (FIFO),
 * false when FULL — the NEWEST item is then dropped, the module
 * drop counter increments, and NOTHING goes on the wire (ruling
 * #3966.2). Never blocks the producer. */
bool control_queue_send_drop_on_full(control_queue_t *q, void *item);

/* Pop the module-static ring head, waiting at most timeout_ms
 * (bounded — never forever; stream.c:79 precedent). True + *out on
 * success; false on timeout. */
bool control_queue_receive_timeout(void **out, uint32_t timeout_ms);

/* Host-only test seam: the module-static ring (capture precedent).
 * Device builds must not call this. */
control_queue_t *control_queue_for_test(void);

/* Drop counter (queue-full + oversize ingests). Logs + getter only —
 * the status-frame schema stays frozen (FW-13.6 untouched). */
uint32_t control_frames_dropped_get(void);

/* Production producer seam (RX hook in ws_server.c): submit an
 * owned heap frame copy to the module-static ring. Takes ownership:
 * true → queued FIFO (the consumer frees exactly once); false →
 * ring FULL — the NEWEST frame is dropped (counter already bumped)
 * and the CALLER frees immediately. Never blocks the producer. */
bool control_frame_submit(void *frame_copy);

/* Record an oversize-ingest drop on the SAME counter as queue-full
 * drops (D3); the caller owns the distinct log line. */
void control_dropped_frame_record(void);

/* One consumer iteration: bounded pop → process → emit the reply
 * envelope via ws_sink_send_text (0-sentinel skips the send) →
 * free the frame copy (single owner, free-once). Device task body
 * loops this forever; host tests drive iterations manually. */
bool control_loop_iteration(void);

/* Strong symbol replacing the FW-03-era boot stub: host records the
 * supervision bookkeeping verbatim; device spawns the "control"
 * task (4096/PRIO 5, boot_priq.h). Arms the ring sync hooks BEFORE
 * spawning (capture.c:246 precedent). */
esp_err_t control_task_start(void);

/* Host-only test seam: clears the registry + counters so suites stay
 * order-independent. No-op on device (BSS-zeroed at boot). */
void control_reset_for_test(void);

#ifdef __cplusplus
}
#endif
