/* test_stream_fragment.c — FW-15.2 pure fragment-planner scenarios
 * (REQ-ST-003 / REQ-ST-004).
 *
 * The planner is PURE and array-free (design D3):
 *
 *   size_t stream_fragment_count(size_t len, size_t chunk)
 *   size_t stream_fragment_offset(size_t i,  size_t len, size_t chunk)
 *
 * Chunk comes straight from CONFIG_FIRMWARE_WS_BUFFER_SIZE
 * (= 16384, mirrored as a host cflag at run_host_tests.py) — no
 * getter seam, no new Kconfig symbol.
 *
 * Part-count table under test (chunk = 16384):
 *
 *   len      → parts        basis
 *   --------+------         ------------------------------------
 *   8000    → 1            well under one chunk
 *   16000   → 1            still ≤ chunk (REQ-ST-001 single send)
 *   16001   → 1            ≤ chunk — see SPEC-NOTE below
 *   32768   → 2            exactly 2 × chunk
 *   32769   → 3            one byte over 2 × chunk
 *   16384   → 1            exact-chunk boundary
 *   0       → 0            degenerate guard
 *
 * SPEC-NOTE (deviation from the tasks/spec artifact, flagged for
 * the verify phase): the milestone table lists 16001 → 2. That
 * row contradicts REQ-ST-001 ("a frame with length ≤ chunk size
 * SHALL be delivered as exactly ONE binary WebSocket DATA event")
 * since 16001 < 16384. A uniform-capacity model reproducing it
 * does not exist: thresholds t₁ = 16000 and t₂ = 32768 imply
 * t₂ − t₁ = 16768 > chunk, so any 2-part plan honouring BOTH
 * boundaries must emit one fragment larger than the WS TX buffer,
 * which fails on hardware. We implement the only model consistent
 * with REQ-ST-001 + REQ-ST-004: ceil(len / chunk).
 */

#include "stream_fragment.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* The host cflag mirror of the device Kconfig default. */
#define CHUNK CONFIG_FIRMWARE_WS_BUFFER_SIZE

/* ---------- S1 — REQ-ST-003/004 part-count table ---------- */
TEST_CASE(
    "test_fw15_part_count_table_matches_chunk_boundaries [fw-15.2][req-st-003][scenario-S1]",
    "[stream][fw-15.2][planner]")
{
    /* Sanity: the knob the planner reads is the WS buffer size. */
    TEST_ASSERT_EQUAL_INT(16384, CHUNK);

    TEST_ASSERT_EQUAL_size_t(1, stream_fragment_count(8000,  CHUNK));
    TEST_ASSERT_EQUAL_size_t(1, stream_fragment_count(16000, CHUNK));
    TEST_ASSERT_EQUAL_size_t(1, stream_fragment_count(16001, CHUNK));
    TEST_ASSERT_EQUAL_size_t(2, stream_fragment_count(32768, CHUNK));
    TEST_ASSERT_EQUAL_size_t(3, stream_fragment_count(32769, CHUNK));
}

/* ---------- S2 — triangulation: exact boundaries + degenerate ---------- */
TEST_CASE(
    "test_fw15_part_count_boundary_and_degenerate_rows [fw-15.2][req-st-003][scenario-S2]",
    "[stream][fw-15.2][planner]")
{
    TEST_ASSERT_EQUAL_size_t(1, stream_fragment_count(CHUNK,     CHUNK));
    TEST_ASSERT_EQUAL_size_t(2, stream_fragment_count(CHUNK + 1, CHUNK));
    TEST_ASSERT_EQUAL_size_t(0, stream_fragment_count(0,         CHUNK));
    /* Degenerate chunk guard: never divide by zero. */
    TEST_ASSERT_EQUAL_size_t(0, stream_fragment_count(100, 0));
}

/* ---------- S3 — byte-exact partition via offsets ---------- */
TEST_CASE(
    "test_fw15_fragment_offsets_partition_len_exactly [fw-15.2][req-st-003][scenario-S3]",
    "[stream][fw-15.2][reassembly]")
{
    const size_t lens[] = { 8000, 16000, 16001, CHUNK, CHUNK + 1, 32768, 32769 };
    for (size_t row = 0; row < sizeof(lens) / sizeof(lens[0]); row++) {
        size_t len = lens[row];
        size_t n   = stream_fragment_count(len, CHUNK);
        size_t covered = 0;
        for (size_t i = 0; i < n; i++) {
            size_t off = stream_fragment_offset(i, len, CHUNK);
            TEST_ASSERT_EQUAL_size_t(covered, off);
            size_t part = len - off;
            if (part > CHUNK) part = CHUNK;
            covered += part;
        }
        /* Concatenation of [off_i, off_i + part_i) covers every
         * byte exactly once — no gaps, no overlaps. */
        TEST_ASSERT_EQUAL_size_t(len, covered);
    }
}

/* ---------- S4 — out-of-range index clamps to len ---------- */
TEST_CASE(
    "test_fw15_fragment_offset_out_of_range_clamps_to_len [fw-15.2][req-st-003][scenario-S4]",
    "[stream][fw-15.2][planner]")
{
    TEST_ASSERT_EQUAL_size_t(32768, stream_fragment_offset(2, 32768, CHUNK));
    TEST_ASSERT_EQUAL_size_t(8000,  stream_fragment_offset(9, 8000,  CHUNK));
}
