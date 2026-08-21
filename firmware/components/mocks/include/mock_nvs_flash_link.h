/* mock_nvs_flash_link.h — Linker-override header for NVS API mocks.
 *
 * Include this header in production source files (config.c, main.c)
 * to redirect every `nvs_*` call to the `mock_nvs_*` implementation
 * in `firmware/components/mocks/`.
 *
 * The macro redirection is only active when `MOCK_NVS_USE_REAL` is
 * NOT defined. ESP-IDF's NVS component does NOT define this macro
 * today; the orchestrator-side build (`idf.py build` for a real
 * chip) intentionally does not include this header, so the real
 * `nvs_*` functions are linked instead. Host tests (`idf.py test
 * --target esp32` with `tests.host.mocks` declared in
 * `idf_component.yml`) DO include this header and link the mocks.
 *
 * Usage in production source:
 *   #include "mock_nvs_flash_link.h"   // before <nvs.h>
 *   #include <nvs.h>
 *
 * The macro redirect is the same trick CMock and cpputest use; it
 * works because the function signatures are identical between the
 * mock and the real driver, and the linker resolves the symbol
 * after preprocessing.
 */
#pragma once

#include "mock_nvs_flash.h"

#ifndef MOCK_NVS_USE_REAL

#define nvs_open(name, mode, out)             mock_nvs_open(name, mode, out)
#define nvs_close(handle)                     mock_nvs_close(handle)
#define nvs_set_str(handle, key, value)       mock_nvs_set_str(handle, key, value)
#define nvs_get_str(handle, key, out, len)    mock_nvs_get_str(handle, key, out, len)
#define nvs_set_u8(handle, key, value)        mock_nvs_set_u8(handle, key, value)
#define nvs_get_u8(handle, key, out)          mock_nvs_get_u8(handle, key, out)
#define nvs_commit(handle)                    mock_nvs_commit(handle)
#define nvs_erase_key(handle, key)            mock_nvs_erase_key(handle, key)
#define nvs_erase_all(handle)                 mock_nvs_erase_all(handle)

#define nvs_flash_init()                      mock_nvs_flash_init()
#define nvs_flash_erase()                     mock_nvs_flash_erase()
#define nvs_get_stats(part, stats)            mock_nvs_get_stats(part, stats)

#endif  /* !MOCK_NVS_USE_REAL */