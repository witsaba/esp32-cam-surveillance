/* esp_log.h — host stub of IDF's esp_log.h.
 *
 * The real IDF header pulls in sdkconfig.h, esp_rom_sys.h, and the
 * FreeRTOS port — none of which exist on a plain gcc host. We
 * provide a no-op stub so `config.c`'s `ESP_LOGW(...)` macro
 * compiles away on host. The mock test still observes the warning
 * indirectly through the version-mismatch assertion (no log capture
 * required for the FW-02.1 walk; the FW-02.3 bite-proof test instead
 * asserts that the named test fails when the version check is
 * stubbed).
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

#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

#endif /* HOST_ESP_LOG_H */