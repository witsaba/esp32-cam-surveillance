/* host_test_main.c — FW-02 host-side Unity test driver.
 *
 * Provides the `main()` that Unity needs when run outside the IDF test
 * runner. The TEST_CASE macros in the test files use IDF's
 * `unity_test_runner.h` extensions, which auto-register each test via
 * a constructor via `unity_testcase_register()`. The host shim
 * (`host_idf_runner_shim.c`) provides that function and the linked
 * list head. This driver walks the list and runs each test.
 *
 * No FreeRTOS, no IDF runtime. The whole point of the host test
 * runner is to verify the production code's logic without flashing.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

/* Mirror IDF's test_desc_t. The shim and the host driver agree on
 * this layout. */
struct test_desc_t {
    const char *name;
    const char *desc;
    void (**fn)(void);
    const char *file;
    int line;
    uint8_t test_fn_count;
    const char **test_fn_name;
    struct test_desc_t *next;
};

extern struct test_desc_t *s_unity_tests_first;

void setUp(void) { /* no-op */ }
void tearDown(void) { /* no-op */ }

int main(void)
{
    printf("FW-02 host test runner — invoking Unity.\n");
    fflush(stdout);

    int total = 0;
    int failures = 0;

    for (struct test_desc_t *t = s_unity_tests_first; t != NULL; t = t->next) {
        total++;
        printf("RUN  [%d] %s\n", total, t->name);
        fflush(stdout);
        UnityBegin(t->file);
        /* Wrap the test body in TEST_PROTECT so assertion failures
         * (which longjmp via Unity.AbortFrame) return control here
         * instead of crashing the binary. */
        if (setjmp(Unity.AbortFrame) == 0) {
            if (t->fn && t->fn[0]) {
                t->fn[0]();
            }
        }
        /* IDF's unity_default_test_run() calls UnityConcludeTest()
         * here — the function that bumps Unity.TestFailures based
         * on Unity.CurrentTestFailed. Without this, UnityEnd()
         * reports 0 failures even when assertions failed. */
        UnityConcludeTest();
        int failed = UnityEnd();
        if (failed > 0) {
            failures++;
            printf("FAIL [%d] %s (%d assertion failure(s))\n",
                   total, t->name, failed);
        } else {
            printf("PASS [%d] %s\n", total, t->name);
        }
        fflush(stdout);
    }

    printf("\n=== FW-02 host tests: %d total, %d failed ===\n",
           total, failures);
    return failures > 0 ? 1 : 0;
}