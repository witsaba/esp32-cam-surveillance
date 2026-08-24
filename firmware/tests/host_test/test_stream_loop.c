/* test_stream_loop.c — FW-15.1/15.2/15.3 host scenarios for the
 * stream component surface.
 *
 * FW-15.1 (this file, first batch): REQ-ST-006 bounded-timeout
 * receive. The stream task consumes the depth-2 capture queue via
 * `capture_queue_receive_timeout(out, timeout_ms)` which must NEVER
 * block forever: an empty queue returns false within approximately
 * `timeout_ms` (measured against CLOCK_MONOTONIC).
 *
 * Scenarios:
 *   S1 (REQ-ST-006) — empty queue + T=50 ms → false within ≈T ms.
 *   S2 (REQ-ST-006 triangulation) — queued item → true immediately,
 *       out carries the exact pointer pushed by the producer.
 *   S3 (REQ-ST-006 cross-thread) — producer thread pushes after a
 *       delay; a waiting consumer wakes EARLY (well under its full
 *       timeout) with the item. Proves the condvar/semaphore signal
 *       path is live, not just the timeout path.
 *
 * Conventions mirror test_capture_loop.c: reset the mock triplet +
 * the capture module-statics per test, then drive the public API.
 * The sync hooks are installed on the module-static queue by
 * `capture_task_start()` (design D2) — stack-instantiated queues
 * keep NULL hooks and stay lock-free.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_supervision_record.h"
#include "mock_init_returns.h"
#include "boot.h"
#include "unity.h"

/* FW-15.2 sender surface. */
#include "stream_fragment.h"
#include "stream.h"
#include "ws.h"
#include "config.h"
#include "mock_esp_websocket_client_link.h"
#include "mock_esp_event_link.h"
#include "mock_esp_system_link.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <pthread.h>

/* Milliseconds elapsed between two CLOCK_MONOTONIC samples. */
static uint32_t elapsed_ms_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t ns = (int64_t)(t1.tv_sec - t0->tv_sec) * 1000000000LL
               + (int64_t)(t1.tv_nsec - t0->tv_nsec);
    if (ns < 0) ns = 0;
    return (uint32_t)(ns / 1000000LL);
}

/* Standard fixture: fresh mocks + fresh module-static queue, then
 * arm the sync hooks exactly like a real boot does. */
static void stream_with_mocks(void)
{
    mock_esp_camera_reset();
    mock_supervision_reset();
    mock_init_returns_reset();
    capture_counters_reset_for_test();
    /* Host capture_task_start records the supervision role AND
     * installs the mutex/condvar hooks on g_capture_queue. */
    TEST_ASSERT_EQUAL(ESP_OK, capture_task_start());
}

/* ---------- S1 — REQ-ST-006: empty queue times out bounded ---------- */
TEST_CASE(
    "test_fw15_empty_queue_receive_times_out_bounded [fw-15.1][req-st-006][scenario-S1]",
    "[stream][fw-15.1][timeout]")
{
    stream_with_mocks();

    void *out = (void *)0xDEADBEEF;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool got = capture_queue_receive_timeout(&out, 50);
    uint32_t elapsed = elapsed_ms_since(&t0);

    TEST_ASSERT_FALSE_MESSAGE(got,
        "empty queue must report 'no item' via false");
    /* ≈T: never blocks forever, never returns instantly-spurious.
     * Generous upper bound keeps CI flakes away; the lower bound
     * proves the wait actually happened. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(500, elapsed);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(40, elapsed);
}

/* ---------- S2 — REQ-ST-006 triangulation: item present → true ---------- */
TEST_CASE(
    "test_fw15_queued_item_received_with_exact_pointer [fw-15.1][req-st-006][scenario-S2]",
    "[stream][fw-15.1][receive]")
{
    stream_with_mocks();

    camera_fb_t fb = { .buf = (uint8_t *)0x1234, .len = 8000 };
    /* capture_queue_for_test() exposes the module-static ring (host
     * test seam, mirrors capture_counters_reset_for_test). Hooks are
     * armed by capture_task_start(), so this push locks + signals. */
    TEST_ASSERT_TRUE(capture_queue_send_drop_on_full(
        capture_queue_for_test(), &fb));

    void *out = NULL;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool got = capture_queue_receive_timeout(&out, 1000);
    uint32_t elapsed = elapsed_ms_since(&t0);

    TEST_ASSERT_TRUE_MESSAGE(got, "queued item must be received");
    TEST_ASSERT_EQUAL_PTR(&fb, out);
    /* Must return promptly (signalled path), not burn the timeout. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(100, elapsed);
}

/* ---------- S3 — cross-thread producer wakes a waiting consumer ---------- */

static pthread_mutex_t s_producer_lock = PTHREAD_MUTEX_INITIALIZER;
static camera_fb_t     s_producer_fb;
static bool            s_producer_ran;

static void *producer_thread(void *arg)
{
    (void)arg;
    /* Let the consumer settle into its wait first. */
    usleep(30 * 1000);
    pthread_mutex_lock(&s_producer_lock);
    s_producer_fb.buf = (uint8_t *)0xBEEF;
    s_producer_fb.len = 11520;
    bool ok = capture_queue_send_drop_on_full(
        capture_queue_for_test(), &s_producer_fb);
    s_producer_ran = ok;
    pthread_mutex_unlock(&s_producer_lock);
    return NULL;
}

TEST_CASE(
    "test_fw15_waiting_consumer_wakes_on_producer_push [fw-15.1][req-st-006][scenario-S3]",
    "[stream][fw-15.1][cross-thread]")
{
    stream_with_mocks();

    s_producer_ran = false;
    pthread_t th;
    TEST_ASSERT_EQUAL(0, pthread_create(&th, NULL, producer_thread, NULL));

    void *out = NULL;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool got = capture_queue_receive_timeout(&out, 2000);
    uint32_t elapsed = elapsed_ms_since(&t0);

    pthread_join(th, NULL);

    TEST_ASSERT_TRUE_MESSAGE(s_producer_ran,
        "producer push must succeed into the synced queue");
    TEST_ASSERT_TRUE_MESSAGE(got, "consumer must wake with the item");
    TEST_ASSERT_EQUAL_PTR(&s_producer_fb, out);
    /* Woke on the ~30 ms push, NOT on the 2000 ms timeout. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1000, elapsed);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(20, elapsed);
}

/* ==================== FW-15.2 sender scenarios ==================== */

/* Sender fixture: fresh mocks + a REAL ws_init pass so the handle
 * is bound exactly like production (ws.c's s_ws_handle is a
 * different static than the event-handler TU's — only ws_init
 * sets both). Mirrors test_ws_hello_first_frame.c reset_state. */
static void stream_sender_with_mocks(void)
{
    mock_esp_camera_reset();
    mock_supervision_reset();
    mock_init_returns_reset();
    mock_esp_websocket_client_reset_for_test();
    mock_esp_event_reset();
    capture_counters_reset_for_test();
    ws_event_handler_reset_for_test();

    uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0x9d, 0x50, 0x08};
    mock_esp_read_mac_set_bytes(mac);

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.wifi.ssid, "TestSSID", sizeof(cfg.wifi.ssid) - 1);
    TEST_ASSERT_EQUAL(ESP_OK, ws_init(&cfg));
}

/* ---------- S5 — REQ-ST-001/002: 8 KB frame → ONE binary event ---------- */
TEST_CASE(
    "test_fw15_8k_frame_ships_as_single_binary_event [fw-15.2][req-st-001][scenario-S5]",
    "[stream][fw-15.2][single-send]")
{
    stream_sender_with_mocks();

    static uint8_t frame[8192];
    for (size_t i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)(i & 0xFF);

    int rc = stream_send_frame(frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT((int)sizeof(frame), rc);

    /* Exactly ONE send verb fired, and it was send_bin. */
    TEST_ASSERT_EQUAL_size_t(1, mock_esp_websocket_client_send_bin_call_count());
    TEST_ASSERT_EQUAL_size_t(0, mock_esp_websocket_client_send_bin_partial_call_count());
    TEST_ASSERT_EQUAL_size_t(0, mock_esp_websocket_client_send_cont_msg_call_count());
    TEST_ASSERT_EQUAL_size_t(0, mock_esp_websocket_client_send_fin_call_count());

    /* The single recorded frame: opcode 0x2 (binary), FIN=1,
     * byte-exact payload. */
    uint8_t op = 0xFF; bool fin = false;
    static uint8_t out[MOCK_WS_BIN_PART_CAP];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, mock_esp_websocket_client_get_bin_frame_at(
        0, &op, &fin, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL_HEX8(0x2, op);
    TEST_ASSERT_TRUE(fin);
    TEST_ASSERT_EQUAL_size_t(sizeof(frame), out_len);
    TEST_ASSERT_EQUAL_MEMORY(frame, out, sizeof(frame));
}

/* ---------- S6 — REQ-ST-003: oversized frame fragments byte-exactly ---------- */
TEST_CASE(
    "test_fw15_oversized_frame_fragments_byte_exact [fw-15.2][req-st-003][scenario-S6]",
    "[stream][fw-15.2][fragment-send]")
{
    stream_sender_with_mocks();

    /* 20000 bytes @ chunk 16384 → 2 parts:
     * partial(16384) + fin(3616). */
    static uint8_t big[20000];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)((i * 7) & 0xFF);

    int rc = stream_send_frame(big, sizeof(big));
    TEST_ASSERT_EQUAL_INT((int)sizeof(big), rc);

    /* Mapping for N=2: partial → cont → fin (send_fin carries no
     * payload in IDF v1.8.0 — the last payload slice rides a
     * continuation). */
    TEST_ASSERT_EQUAL_size_t(0, mock_esp_websocket_client_send_bin_call_count());
    TEST_ASSERT_EQUAL_size_t(1, mock_esp_websocket_client_send_bin_partial_call_count());
    TEST_ASSERT_EQUAL_size_t(1, mock_esp_websocket_client_send_cont_msg_call_count());
    TEST_ASSERT_EQUAL_size_t(1, mock_esp_websocket_client_send_fin_call_count());

    /* Reassemble the ring in order: partial (opcode 0x2, FIN=0),
     * continuation (opcode 0x0, FIN=0), then the empty FIN
     * terminator (opcode 0x0, FIN=1). Concatenation must equal
     * the original bytes. */
    static uint8_t reassembled[sizeof(big)];
    size_t total = 0;
    uint8_t op; bool fin; size_t flen;
    static uint8_t part[MOCK_WS_BIN_PART_CAP];

    TEST_ASSERT_EQUAL(ESP_OK, mock_esp_websocket_client_get_bin_frame_at(
        0, &op, &fin, part, sizeof(part), &flen));
    TEST_ASSERT_EQUAL_HEX8(0x2, op);
    TEST_ASSERT_FALSE(fin);
    TEST_ASSERT_EQUAL_size_t(CONFIG_FIRMWARE_WS_BUFFER_SIZE, flen);
    memcpy(reassembled + total, part, flen); total += flen;

    TEST_ASSERT_EQUAL(ESP_OK, mock_esp_websocket_client_get_bin_frame_at(
        1, &op, &fin, part, sizeof(part), &flen));
    TEST_ASSERT_EQUAL_HEX8(0x0, op);
    TEST_ASSERT_FALSE(fin);
    TEST_ASSERT_EQUAL_size_t(sizeof(big) - total, flen);
    memcpy(reassembled + total, part, flen); total += flen;

    TEST_ASSERT_EQUAL(ESP_OK, mock_esp_websocket_client_get_bin_frame_at(
        2, &op, &fin, NULL, 0, &flen));
    TEST_ASSERT_EQUAL_HEX8(0x0, op);
    TEST_ASSERT_TRUE(fin);
    TEST_ASSERT_EQUAL_size_t(0, flen);

    TEST_ASSERT_EQUAL_size_t(total, sizeof(big));
    TEST_ASSERT_EQUAL_MEMORY(big, reassembled, sizeof(big));
}
