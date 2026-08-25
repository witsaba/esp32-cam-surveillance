/* test_control_route.c — FW-18 pure router core suite (U1).
 *
 * Transcribes spec #3970 scenarios into host assertions against the
 * PURE core only (no httpd/ws mock linkage — "pure-core isolation"):
 *
 *   S1  six-command allow-list routing → not_implemented (FW-18.1/R5)
 *   S2  unknown command rejected with lossless string id echo (FW-18.3)
 *   S3  numeric id preserved unquoted, same type (FW-18.3/#3966.4)
 *   S4  garbage body → bad_json without recoverable id (FW-18.4/#3966.3)
 *   S5  garbage body WITH salvageable string id → both branches (D2)
 *   S6  garbage body WITH salvageable numeric id (D2)
 *   S7  valid JSON with unusable cmd → unknown (FW-18.4)
 *   S8  id omitted when absent/object/null (D9/ruling #3966.8 reading)
 *   S9  malformed input never reaches a registered setter (FW-18.4)
 *   S10 allow-listed command WITH a registered handler dispatches
 *       through the registry seam and emits NO envelope (FW-19+ plug-in)
 */
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

#include <string.h>

#include "esp_err.h"
#include "control.h"

/* ---------- fixtures ---------- */

static int s_probe_calls;

static esp_err_t probe_handler(const char *body, size_t len, void *ctx)
{
    (void)ctx;
    s_probe_calls++;
    /* Record that a body REACHED a setter slot — length observable
     * so the guard can also assert what was handed over. */
    return (body && len > 0) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void route_reset(void)
{
    control_reset_for_test();
    s_probe_calls = 0;
}

/* Process helper — runs one frame through the core and NUL-terminates
 * the envelope for byte-exact comparison. Returns bytes written. */
static size_t process(const char *body, char *out, size_t cap)
{
    memset(out, 0xAA, cap);
    size_t n = control_frame_process(body, strlen(body), out, cap - 1);
    out[n < cap ? n : cap - 1] = '\0';
    return n;
}

/* ---------- S1: allow-list routing (six commands, FW-18 state) ---------- */
TEST_CASE(
    "test_route_six_commands_all_not_implemented [fw-18.1][scenario-outline]",
    "[control][fw-18.1]")
{
    route_reset();
    static const char *cmds[CONTROL_CMD_COUNT] = {
        "stream", "config", "reset_cam", "sleep", "reboot", "identify",
    };
    for (int i = 0; i < CONTROL_CMD_COUNT; ++i) {
        char body[64];
        snprintf(body, sizeof(body), "{\"cmd\":\"%s\",\"id\":\"k\"}",
                 cmds[i]);
        char out[CONTROL_FRAME_MAX];
        size_t n = process(body, out, sizeof(out));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(
            "{\"type\":\"error\",\"reason\":\"not_implemented\","
            "\"id\":\"k\"}",
            out, cmds[i]);
        TEST_ASSERT_EQUAL_INT(
            (int)strlen("{\"type\":\"error\",\"reason\":"
                        "\"not_implemented\",\"id\":\"k\"}"),
            (int)n);
    }
    TEST_ASSERT_EQUAL_INT(0, s_probe_calls);
}

/* ---------- S2: unknown command + string id echo ---------- */
TEST_CASE(
    "test_route_unknown_command_rejected_with_string_echo [fw-18.3]",
    "[control][fw-18.3]")
{
    route_reset();
    char out[CONTROL_FRAME_MAX];
    process("{\"cmd\":\"something_else\",\"id\":\"x1\"}",
            out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\",\"id\":\"x1\"}", out);
}

/* ---------- S3: numeric id preserved unquoted ---------- */
TEST_CASE(
    "test_route_numeric_id_preserved_unquoted [fw-18.3][ruling-4]",
    "[control][fw-18.3]")
{
    route_reset();
    char out[CONTROL_FRAME_MAX];
    process("{\"cmd\":\"frobnicate\",\"id\":42}", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\",\"id\":42}", out);
}

/* ---------- S4: garbage without recoverable id ---------- */
TEST_CASE(
    "test_route_garbage_bad_json_without_id [fw-18.4]",
    "[control][fw-18.4]")
{
    route_reset();
    char out[CONTROL_FRAME_MAX];
    process("this is not json at all", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"bad_json\"}", out);
    TEST_ASSERT_EQUAL_INT(0, s_probe_calls);
}

/* ---------- S5: garbage WITH salvageable string id (branch B) ---------- */
TEST_CASE(
    "test_route_garbage_salvages_string_id [fw-18.4][salvage]",
    "[control][fw-18.4]")
{
    route_reset();
    char out[CONTROL_FRAME_MAX];
    process("oops {\"id\":\"zz\"} trailing", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"bad_json\",\"id\":\"zz\"}",
        out);
}

/* ---------- S6: garbage WITH salvageable numeric id ---------- */
TEST_CASE(
    "test_route_garbage_salvages_numeric_id [fw-18.4][salvage]",
    "[control][fw-18.4]")
{
    route_reset();
    char out[CONTROL_FRAME_MAX];
    process("[1,2 {\"id\":7}", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"bad_json\",\"id\":7}", out);
}

/* ---------- S7: valid JSON, unusable cmd → unknown ---------- */
TEST_CASE(
    "test_route_valid_json_unusable_cmd_is_unknown [fw-18.4]",
    "[control][fw-18.4]")
{
    route_reset();
    char out[CONTROL_FRAME_MAX];

    process("{\"foo\":1}", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\"}", out);

    process("{\"cmd\":123,\"id\":\"q\"}", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\",\"id\":\"q\"}",
        out);
}

/* ---------- S8: id omitted when absent / object / null ---------- */
TEST_CASE(
    "test_route_id_omitted_when_absent_object_or_null [fw-18.4][d9]",
    "[control][fw-18.4]")
{
    route_reset();
    char out[CONTROL_FRAME_MAX];

    process("{\"cmd\":\"frobnicate\"}", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\"}", out);

    process("{\"cmd\":\"frobnicate\",\"id\":{\"a\":1}}",
            out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\"}", out);

    process("{\"cmd\":\"frobnicate\",\"id\":null}",
            out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\"}", out);

    /* Same omission rule on the bad_json path (uniform across tokens,
     * D9). */
    process("garbage {\"id\":[1,2]}", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"bad_json\"}", out);
}

/* ---------- S9: malformed input never reaches a setter ---------- */
TEST_CASE(
    "test_route_malformed_input_never_invokes_setter [fw-18.4][guard-prod]",
    "[control][fw-18.4]")
{
    route_reset();
    control_handler_register(CONTROL_CMD_STREAM, probe_handler);

    char out[CONTROL_FRAME_MAX];
    process("{{{definitely not json", out, sizeof(out));
    process("{\"cmd\":123,\"id\":1}", out, sizeof(out));
    process("{\"cmd\":\"stream\"", out, sizeof(out)); /* truncated */

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_probe_calls,
        "validate-before-setter: malformed bodies must not reach "
        "registered handlers");

    /* A VALID allow-listed body still dispatches normally. */
    process("{\"cmd\":\"stream\",\"id\":\"ok\"}", out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(1, s_probe_calls);
}

/* ---------- S10: registered handler path emits no envelope ---------- */
TEST_CASE(
    "test_route_registered_handler_dispatches_no_envelope [fw-18.1][seam]",
    "[control][fw-18.1]")
{
    route_reset();
    control_handler_register(CONTROL_CMD_IDENTIFY, probe_handler);

    char out[CONTROL_FRAME_MAX];
    size_t n = process("{\"cmd\":\"identify\",\"id\":\"z\"}",
                       out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(1, s_probe_calls);
    TEST_ASSERT_EQUAL_INT(0, (int)n); /* nothing to send downstream */

    /* Unregistered siblings stay not_implemented. */
    process("{\"cmd\":\"stream\",\"id\":\"z\"}", out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(1, s_probe_calls);
    TEST_ASSERT_TRUE(strstr(out, "not_implemented") != NULL);
}

/* ---------- S11: string ids pass the bounded escaper (D6) ---------- */
TEST_CASE(
    "test_route_string_id_escapes_quote_backslash_control [fw-18.3][d6]",
    "[control][fw-18.3]")
{
    route_reset();
    char out[CONTROL_FRAME_MAX];

    /* Raw body carries JSON escapes; the envelope re-escapes them so
     * the reply stays well-formed: " → \" , \ → \\ , \n → \u000a. */
    process("{\"cmd\":\"frobnicate\",\"id\":\"a\\\"b\\\\c\\nd\"}",
            out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\","
        "\"id\":\"a\\\"b\\\\c\\u000ad\"}",
        out);
}

/* ---------- S12: echo cap — ≤64 verbatim, >64 omitted (#3966.8) --- */
TEST_CASE(
    "test_route_echo_cap_boundary_and_omission [fw-18.3][ruling-8]",
    "[control][fw-18.3]")
{
    route_reset();
    /* Exactly-64-char id (8 × 8 'a'). */
    static const char id64[] =
        "aaaaaaaa" "aaaaaaaa" "aaaaaaaa" "aaaaaaaa"
        "aaaaaaaa" "aaaaaaaa" "aaaaaaaa" "aaaaaaaa";
    char body[128];
    char out[CONTROL_FRAME_MAX];
    char expect[CONTROL_FRAME_MAX];

    /* Boundary: 64 chars echo verbatim. */
    TEST_ASSERT_EQUAL_INT(64, (int)strlen(id64));
    snprintf(body, sizeof(body),
             "{\"cmd\":\"frobnicate\",\"id\":\"%s\"}", id64);
    process(body, out, sizeof(out));
    snprintf(expect, sizeof(expect),
             "{\"type\":\"error\",\"reason\":\"unknown\",\"id\":\"%s\"}",
             id64);
    TEST_ASSERT_EQUAL_STRING(expect, out);

    /* Over-cap: 65 chars → id member OMITTED entirely (never
     * truncated into a lie, never null). */
    snprintf(body, sizeof(body),
             "{\"cmd\":\"frobnicate\",\"id\":\"%sa\"}", id64);
    process(body, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\"}", out);
}

/* ---------- S13: overflow → 0 sentinel (no frame emitted) -------- */
TEST_CASE(
    "test_route_envelope_overflow_returns_zero_sentinel [fw-18.3][d6]",
    "[control][fw-18.3]")
{
    route_reset();
    char small[16];
    size_t n = process("{\"cmd\":\"frobnicate\",\"id\":42}",
                       small, sizeof(small));
    TEST_ASSERT_EQUAL_INT(0, (int)n); /* caller skips the send */

    /* The pipeline stays healthy after an overflow skip. */
    char big[CONTROL_FRAME_MAX];
    n = process("{\"cmd\":\"frobnicate\",\"id\":42}", big, sizeof(big));
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"error\",\"reason\":\"unknown\",\"id\":42}", big);
}
