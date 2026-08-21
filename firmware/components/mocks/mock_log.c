/* mock_log.c — host-only capture buffers for ESP_LOG*. */
#include "mock_log.h"

#include <string.h>

char mock_log_last_error[MOCK_LOG_LAST_LEN] = "";
char mock_log_last_warn [MOCK_LOG_LAST_LEN] = "";
char mock_log_last_info [MOCK_LOG_LAST_LEN] = "";
size_t mock_log_error_count = 0;
size_t mock_log_warn_count  = 0;
size_t mock_log_info_count  = 0;

void mock_log_reset(void) {
    mock_log_last_error[0] = '\0';
    mock_log_last_warn[0]  = '\0';
    mock_log_last_info[0]  = '\0';
    mock_log_error_count = 0;
    mock_log_warn_count  = 0;
    mock_log_info_count  = 0;
}