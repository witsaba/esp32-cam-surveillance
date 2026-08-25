/* ws_sink_recorder.h — host-test recorder for the FW-16 viewer
 * sink seam.
 *
 * Installs a ws_sink_t whose sends append frames to in-memory
 * rings instead of touching any socket. Lets stream/status/hello
 * suites assert byte-exact payloads and emission counts without
 * the httpd mock. The fail-all switch simulates a dead socket
 * for the D4 drain-drop-count scenarios.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Reset all recorded state + failure injection, then install the
 * recorder as the ACTIVE sink via ws_sink_install(). */
esp_err_t ws_sink_recorder_install(void);

/* Clear rings + failure flag WITHOUT reinstalling anything
 * (leaves the previously installed sink pointer alone). */
void ws_sink_recorder_reset(void);

/* Dead-socket simulation: when set, every send returns
 * ESP_FAIL and records NOTHING. */
void ws_sink_recorder_fail_all_set(bool fail_all);

size_t    ws_sink_recorder_text_count(void);
size_t    ws_sink_recorder_bin_count(void);

/* Copy text frame `idx` into `out` (NUL-terminated). */
esp_err_t ws_sink_recorder_get_text_at(size_t idx, char *out,
                                        size_t cap);

/* Copy binary frame `idx` into `out`; actual length in *len. */
esp_err_t ws_sink_recorder_get_bin_at(size_t idx, uint8_t *out,
                                        size_t cap, size_t *len);

/* Lifetime accepted-send total (never decreases on ring eviction):
 * proves NO send was lost once the bounded rings have rotated. */
size_t ws_sink_recorder_frames_total(void);

/* TX serialization invariant: times a send fn was ENTERED while
 * another send was in flight. Zero is the contract (ws.c lock). */
size_t ws_sink_recorder_overlap_violations(void);
