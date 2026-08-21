/* unity_config_host.h — host-side Unity configuration shim.
 *
 * FW-02 host test runner uses the IDF-shipped Unity source but defines
 * its own unity_config.h to avoid pulling in IDF-specific includes
 * (sdkconfig.h, esp_err.h, etc.) that would break a plain gcc build.
 *
 * The shim mirrors the symbols the IDF unity_config.h provides but
 * with host-appropriate defaults. The TEST_ASSERT_* macros are the
 * same — only the IDF-specific extras (UNITY_TEST_PROTECT override,
 * sdkconfig defines) are absent.
 */
#ifndef UNITY_CONFIG_HOST_H
#define UNITY_CONFIG_HOST_H

/* Match IDF defaults: 32-bit int/long/pointer. Unity auto-discovers
 * on 64-bit hosts; the explicit define makes the assertions match
 * what the device side would do. */
#ifndef UNITY_INT_WIDTH
#define UNITY_INT_WIDTH 32
#endif
#ifndef UNITY_LONG_WIDTH
#define UNITY_LONG_WIDTH 32
#endif
#ifndef UNITY_POINTER_WIDTH
#define UNITY_POINTER_WIDTH 32
#endif

/* Math helpers (no esp_err.h on host). */
#include <math.h>
#ifndef __cplusplus
#define UNITY_IS_NAN isnan
#define UNITY_IS_INF isinf
#else
#define UNITY_IS_NAN std::isnan
#define UNITY_IS_INF std::isinf
#endif

/* Mark functions as noreturn on gcc/clang. */
#define UNITY_NORETURN __attribute__((__noreturn__))

#endif /* UNITY_CONFIG_HOST_H */