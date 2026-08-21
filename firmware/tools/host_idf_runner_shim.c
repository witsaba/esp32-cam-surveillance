/* host_idf_runner_shim.c — host-side stub of IDF's unity_runner.c.
 *
 * When CONFIG_UNITY_ENABLE_IDF_TEST_RUNNER is OFF (host build), the
 * `unity_testcase_register()` function declared by IDF's
 * `unity_test_runner.h` is missing. We provide a minimal
 * implementation here: a singly-linked list head plus the register
 * function. The host test driver's main() walks the list and runs
 * each test.
 *
 * Why a stub: keeps the test source files unchanged between host and
 * device builds. The TEST_CASE macros expand the same way, only the
 * registration target differs.
 */
#include <stddef.h>
#include <stdint.h>

/* Mirror IDF's test_desc_t. Must match `unity_test_runner.h`. */
struct test_desc_t {
    const char *name;
    const char *desc;
    void (**fn)(void);          /* test_func* */
    const char *file;
    int line;
    uint8_t test_fn_count;
    const char **test_fn_name;
    struct test_desc_t *next;
};

/* Linked list head; matches the static in IDF's unity_runner.c. */
struct test_desc_t *s_unity_tests_first = NULL;

void unity_testcase_register(struct test_desc_t *desc)
{
    desc->next = s_unity_tests_first;
    s_unity_tests_first = desc;
}