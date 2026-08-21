/* unity_host_test_runner.h — host-side shim of IDF's unity_test_runner.h.
 *
 * FW-02 host test runner needs the TEST_CASE macro to auto-register
 * tests via a constructor, which is the behavior IDF's
 * `components/unity/include/unity_test_runner.h` provides when
 * CONFIG_UNITY_ENABLE_IDF_TEST_RUNNER=y. On host we don't have a
 * sdkconfig.h, so we provide our own copy of the same macro family
 * here — gated by UNITY_HOST_TEST_RUNNER instead of the IDF Kconfig.
 *
 * This file is functionally identical to the IDF header, minus the
 * DISABLED_FOR_TARGETS / TEMPORARY_DISABLED_FOR_TARGETS macros that
 * require the IDF target chip list. We don't need them: the host
 * build always includes every test.
 *
 * The matching registration function (`unity_testcase_register`) is
 * provided by `host_idf_runner_shim.c`.
 */
#ifndef UNITY_HOST_TEST_RUNNER_H
#define UNITY_HOST_TEST_RUNNER_H

#include <stdint.h>
#include <stdbool.h>

#define UNITY_EXPAND2(a, b) a ## b
#define UNITY_EXPAND(a, b) UNITY_EXPAND2(a, b)
#define UNITY_TEST_UID(what) UNITY_EXPAND(what, __LINE__)

#define UNITY_TEST_REG_HELPER reg_helper ## UNITY_TEST_UID
#define UNITY_TEST_DESC_UID desc ## UNITY_TEST_UID

typedef void (* test_func)(void);

typedef struct test_desc_t
{
    const char* name;
    const char* desc;
    test_func* fn;
    const char* file;
    int line;
    uint8_t test_fn_count;
    const char ** test_fn_name;
    struct test_desc_t* next;
} test_desc_t;

void unity_testcase_register(test_desc_t* desc);

#define FN_NAME_SET_1(a)                {#a}
#define FN_NAME_SET_2(a, b)             {#a, #b}
#define FN_NAME_SET_3(a, b, c)          {#a, #b, #c}
#define FN_NAME_SET_4(a, b, c, d)       {#a, #b, #c, #d}
#define FN_NAME_SET_5(a, b, c, d, e)    {#a, #b, #c, #d, #e}

#define FN_NAME_SET2(n) FN_NAME_SET_##n
#define FN_NAME_SET(n, ...) FN_NAME_SET2(n)(__VA_ARGS__)

#define UNITY_TEST_FN_SET(...)  \
    static test_func UNITY_TEST_UID(test_functions)[] = {__VA_ARGS__}; \
    static const char* UNITY_TEST_UID(test_fn_name)[] = FN_NAME_SET(PP_NARG(__VA_ARGS__), __VA_ARGS__)

#define PP_NARG(...) \
         PP_NARG_(__VA_ARGS__,PP_RSEQ_N())
#define PP_NARG_(...) \
         PP_ARG_N(__VA_ARGS__)
#define PP_ARG_N( \
          _1, _2, _3, _4, _5, _6, _7, _8, _9, N, ...) N
#define PP_RSEQ_N() 9,8,7,6,5,4,3,2,1,0

#define TEST_CASE(name_, desc_) \
    static void UNITY_TEST_UID(test_func_) (void); \
    static void __attribute__((constructor)) UNITY_TEST_UID(test_reg_helper_) (void) \
    { \
        static test_func test_fn_[] = {&UNITY_TEST_UID(test_func_)}; \
        static test_desc_t UNITY_TEST_UID(test_desc_) = { \
            .name = name_, \
            .desc = desc_, \
            .fn = test_fn_, \
            .file = __FILE__, \
            .line = __LINE__, \
            .test_fn_count = 1, \
            .test_fn_name = NULL, \
            .next = NULL \
        }; \
        unity_testcase_register( & UNITY_TEST_UID(test_desc_) ); \
    }\
    static void UNITY_TEST_UID(test_func_) (void)

#endif /* UNITY_HOST_TEST_RUNNER_H */