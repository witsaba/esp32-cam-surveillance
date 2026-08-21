/* nvs.h — host stub of IDF's nvs.h (FW-02 host tests).
 *
 * Provides only the open-mode enum + error codes that the production
 * source uses. The real nvs.h lives in $IDF_PATH/components/nvs_flash
 * and pulls in nvs_flash.h, esp_partition.h, sdkconfig.h, etc. —
 * all of which require an IDF build system. On host we just need the
 * open-mode constant for nvs_open().
 */
#ifndef HOST_NVS_H
#define HOST_NVS_H

#include <stdint.h>

/* Match IDF nvs.h types — typedefs only (no enum constants needed
 * on host). The mock_nvs_* functions take int and uint32_t; the
 * IDF header ties these to enum values, but C allows the call site
 * to pass an integer literal that matches the enum value. */
typedef uint32_t nvs_handle_t;
typedef int      nvs_open_mode_t;

/* Mock for nvs_stats_t (used by mock_nvs_get_stats). The IDF struct
 * has more fields, but only these are referenced from FW-02. */
typedef struct {
    uint32_t used_entries;
    uint32_t free_entries;
    uint32_t total_entries;
    uint32_t namespace_count;
} nvs_stats_t;

/* The open-mode constants. Match IDF: NVS_READWRITE = 1. */
#define NVS_READWRITE 1
#define NVS_READONLY  2

#endif /* HOST_NVS_H */