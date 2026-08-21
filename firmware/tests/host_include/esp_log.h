/* esp_log.h — host stub of IDF's esp_log.h.
 *
 * On host, ESP_LOGI/ESP_LOGW/ESP_LOGE write their formatted message
 * into a capture buffer exposed by `mock_log.h` (so the FW-03.2
 * fail-loud bite-proof can assert the orchestrator's log line names
 * the failing step). On device, this file is replaced by IDF's real
 * esp_log.h via the IDF include path.
 */
#ifndef HOST_ESP_LOG_H
#define HOST_ESP_LOG_H

#include "sdkconfig.h"

#define ESP_LOG_LEVEL_NONE     0
#define ESP_LOG_LEVEL_ERROR    1
#define ESP_LOG_LEVEL_WARN     2
#define ESP_LOG_LEVEL_INFO     3
#define ESP_LOG_LEVEL_DEBUG    4
#define ESP_LOG_LEVEL_VERBOSE  5

#include <stdio.h>

#include "mock_log.h"

#define ESP_LOGE(tag, fmt, ...) do { \
    snprintf(mock_log_last_error, MOCK_LOG_LAST_LEN, fmt, ##__VA_ARGS__); \
    mock_log_error_count++; \
} while (0)
#define ESP_LOGW(tag, fmt, ...) do { \
    snprintf(mock_log_last_warn, MOCK_LOG_LAST_LEN, fmt, ##__VA_ARGS__); \
    mock_log_warn_count++; \
} while (0)
#define ESP_LOGI(tag, fmt, ...) do { \
    snprintf(mock_log_last_info, MOCK_LOG_LAST_LEN, fmt, ##__VA_ARGS__); \
    mock_log_info_count++; \
} while (0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

#endif /* HOST_ESP_LOG_H */