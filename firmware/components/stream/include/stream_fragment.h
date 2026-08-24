/* stream_fragment.h — FW-15 pure fragment planner (REQ-ST-003/004).
 *
 * Array-free, allocation-free, no IDF dependencies: the sender
 * (stream_sender.c) maps the plan onto
 * send_bin / send_bin_partial → send_cont_msg* → send_fin.
 *
 * Chunk is passed by the caller — production passes
 * CONFIG_FIRMWARE_WS_BUFFER_SIZE directly (design D3, single knob).
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of fragments needed for `len` payload bytes when each
 * fragment carries at most `chunk` bytes. 0 → 0; ceil(len/chunk)
 * otherwise. A chunk of 0 yields 0 (degenerate guard). */
size_t stream_fragment_count(size_t len, size_t chunk);

/* Byte offset of fragment `i` within the frame. Requires
 * i < stream_fragment_count(len, chunk); an out-of-range or
 * degenerate-chunk query clamps to `len` so callers that compute
 * part = min(chunk, len - offset) naturally terminate. */
size_t stream_fragment_offset(size_t i, size_t len, size_t chunk);

#ifdef __cplusplus
}
#endif
