/* test_control_validate_guard.c — FW-18 U4 validate-before-setter
 * bite-proof guard (firmware-control-dispatcher design D8 #3972;
 * folded into FW-19 U4 / Pass 14, ruling R3).
 *
 * This file compiles in TWO builds:
 *
 *   - PRODUCTION (Pass 1, no flags): pins the validated pipeline —
 *     a garbage body classifies bad_json and NEVER reaches a
 *     registered handler slot.
 *
 *   - STUB BUILD (Pass 14, -DCONTROL_TEST_STUB_SKIP_VALIDATION=1,
 *     applied to BOTH control_route.c AND this file): compiles the
 *     parse/classify validation gate OUT of control_frame_process,
 *     so the registered setter receives the RAW body regardless of
 *     validity — a model of the reference regression where malformed
 *     input flowed straight into setters. The probe then records the
 *     call and this test MUST FAIL with the literal "skip_validation"
 *     in the message. Pass 14 greps for exactly that single expected
 *     failure.
 */

#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "control.h"

#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

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

/* Total-parse-failure body: no JSON prefix whatsoever, so the
 * validated pipeline classifies it bad_json before any registry
 * lookup could happen. */
static const char k_garbage[] = "<<<not-json>>>";

TEST_CASE(
    "test_guard_skip_validation_malformed_body_must_not_reach_setter "
    "[fw-18.4][guard][bite-proof]",
    "[control][fw-18.4][guard]")
{
    control_reset_for_test();
    control_handler_register(CONTROL_CMD_STREAM, probe_handler);
    s_probe_calls = 0;

    char buf[128];
#ifdef CONTROL_TEST_STUB_SKIP_VALIDATION
    /* ---- Pass 14 stub build: the bite target is live. ---- */
    (void)control_frame_process(k_garbage, sizeof(k_garbage) - 1,
                                buf, sizeof(buf));

    /* With the validation gate compiled out, the raw garbage body
     * reaches the registered setter slot. The literal
     * "skip_validation" is what Pass 14 greps for. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, s_probe_calls,
        "skip_validation invariant violated: malformed body reached "
        "the registered setter without validation");
#else
    /* ---- Production build: pin the validated pipeline. ---- */
    size_t n = control_frame_process(k_garbage, sizeof(k_garbage) - 1,
                                     buf, sizeof(buf));
    buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';

    /* Garbage classifies bad_json on the wire... */
    TEST_ASSERT_TRUE_MESSAGE(
        strstr(buf, "\"bad_json\"") != NULL,
        "validate-before-setter prod pin lost: garbage body no "
        "longer classifies bad_json");
    /* ...and the setter is NEVER invoked on unvalidated input. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, s_probe_calls,
        "validate-before-setter prod pin lost: malformed body "
        "reached a registered setter");
#endif
}
