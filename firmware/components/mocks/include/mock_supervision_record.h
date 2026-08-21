/* mock_supervision_record.h — records the call order of the 4
 * supervision-task stubs for the FW-03.1 ordering test. The 4
 * stubs each call `mock_supervision_record("<role>")` before
 * returning (host-only). Reset by `mock_supervision_reset()`
 * between tests.
 */
#pragma once

#include <stddef.h>

#define MOCK_SUPERVISION_RECORD_CAP 16

#ifdef __cplusplus
extern "C" {
#endif

void mock_supervision_reset(void);
void mock_supervision_record(const char *task_name);
size_t mock_supervision_count(void);
size_t mock_supervision_order(size_t idx, char *out_name, size_t cap);

#ifdef __cplusplus
}
#endif