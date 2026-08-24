/* stream_fragment.c — FW-15 pure fragment planner.
 *
 * The plan is arithmetic only: fragments are fixed-width slices
 * of the frame buffer at multiples of `chunk`, with the final
 * fragment carrying the remainder. Zero-copy: the sender slices
 * directly into camera_fb_t.buf via these offsets.
 *
 * Part-count semantics (chunk = CONFIG_FIRMWARE_WS_BUFFER_SIZE):
 *
 *   len == 0        → 0 parts   (caller treats as invalid input)
 *   len ≤ chunk     → 1 part    (single send_bin, REQ-ST-001)
 *   len > chunk     → ceil(len/chunk) parts
 *                     (send_bin_partial → cont* → fin, REQ-ST-003)
 */
#include "stream_fragment.h"

size_t stream_fragment_count(size_t len, size_t chunk)
{
    if (len == 0 || chunk == 0) return 0;
    return (len + chunk - 1) / chunk;
}

size_t stream_fragment_offset(size_t i, size_t len, size_t chunk)
{
    size_t n = stream_fragment_count(len, chunk);
    if (i >= n) return len; /* clamp — see header contract */
    return i * chunk;
}
