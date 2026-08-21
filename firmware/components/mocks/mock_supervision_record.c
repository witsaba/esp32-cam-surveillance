/* mock_supervision_record.c — host-side recorder for supervision-task
 * call order. Each stub in `stub_supervision.c` calls
 * `mock_supervision_record("health"|"capture"|"stream"|"control")`
 * on host. Tests read the recorder with `mock_supervision_count()`
 * and `mock_supervision_order(idx, …)`.
 */
#include "mock_supervision_record.h"

#include <string.h>

static char g_names[MOCK_SUPERVISION_RECORD_CAP][24];
static size_t g_count = 0;

void mock_supervision_reset(void) { g_count = 0; }

void mock_supervision_record(const char *task_name) {
    if (!task_name || g_count >= MOCK_SUPERVISION_RECORD_CAP) return;
    strncpy(g_names[g_count], task_name, sizeof(g_names[0]) - 1);
    g_names[g_count][sizeof(g_names[0]) - 1] = '\0';
    g_count++;
}

size_t mock_supervision_count(void) { return g_count; }

size_t mock_supervision_order(size_t idx, char *out_name, size_t cap) {
    if (idx >= g_count || !out_name || cap == 0) return 0;
    strncpy(out_name, g_names[idx], cap - 1);
    out_name[cap - 1] = '\0';
    return strlen(out_name);
}