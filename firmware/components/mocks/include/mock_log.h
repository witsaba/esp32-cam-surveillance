/* mock_log.h — host-only capture for ESP_LOGE / ESP_LOGW / ESP_LOGI.
 *
 * The host stub of esp_log.h routes every ESP_LOG* call through this
 * capture so the FW-03.2 bite-proof test can assert the orchestrator's
 * error log line contains the failing step's name (e.g. "camera").
 *
 * The capture is a single-line ring: the most-recent message is kept
 * in `mock_log_last_error` (cleared on `mock_log_reset()`). Tests that
 * want to assert a specific log message after a code path runs call
 * `mock_log_reset()`, run the path, then inspect `mock_log_last_error`
 * (or the count).
 *
 * This header is HOST-ONLY. On device builds the real esp_log.h
 * routes to IDF's UART-based logger.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_LOG_LAST_LEN 256

extern char mock_log_last_error[MOCK_LOG_LAST_LEN];
extern char mock_log_last_warn [MOCK_LOG_LAST_LEN];
extern char mock_log_last_info [MOCK_LOG_LAST_LEN];
extern size_t mock_log_error_count;
extern size_t mock_log_warn_count;
extern size_t mock_log_info_count;

void mock_log_reset(void);

#ifdef __cplusplus
}
#endif