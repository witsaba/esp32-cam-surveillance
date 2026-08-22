/* esp_heap.h — host stub of IDF's esp_heap.h (FW-05 host tests).
 *
 * softap_home.c's GET / handler allocates the rendered HTML page on
 * the heap (the httpd worker task's 4096-byte stack can't hold a
 * 2.4 KB page + escaping buffers + printf working space). On host
 * tests the heap allocator is just plain malloc/free. We provide
 * heap_caps_malloc that delegates to malloc with the requested cap
 * flags ignored.
 */
#ifndef HOST_ESP_HEAP_H
#define HOST_ESP_HEAP_H

#include <stdlib.h>
#include "esp_err.h"

/* MALLOC_CAP_* flags — host ignores these, included only to make
 * the production source compile unmodified. */
#define MALLOC_CAP_EXEC       (1 << 0)
#define MALLOC_CAP_32BIT      (1 << 1)
#define MALLOC_CAP_8BIT       (1 << 2)
#define MALLOC_CAP_DMA        (1 << 3)
#define MALLOC_CAP_INTERNAL   (1 << 4)
#define MALLOC_CAP_SPIRAM     (1 << 5)

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(n, size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}

#endif /* HOST_ESP_HEAP_H */
