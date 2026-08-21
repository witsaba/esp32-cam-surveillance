/* mock_nvs_flash.cpp — In-memory implementation of the NVS API mocks
 * declared in mock_nvs_flash.h.
 *
 * Storage model:
 *   - Process-global `static std::map<std::string, Namespace>` keyed
 *     by namespace name.
 *   - Each Namespace is a `std::map<std::string, Entry>` keyed by NVS
 *     key name; Entry holds a `std::vector<uint8_t>` (typed payload,
 *     length-prefixed for strings) plus a `nvs_type_t` tag.
 *
 * Concurrency: not thread-safe. ESP-IDF's NVS driver is internally
 * thread-safe, but tests run on a single thread by default. If a
 * future test runs the mock from multiple tasks, wrap each call in
 * a mutex (or upgrade to `std::shared_mutex`).
 *
 * Device builds (`MOCK_NVS_USE_REAL` defined): the linker-override
 * macros in `mock_nvs_flash_link.h` are NOT activated, so the real
 * NVS driver is linked and this file's mock_nvs_* symbols are dead
 * code (still compiled because the mock component is registered,
 * but harmless because the linker never resolves them).
 */
#include "mock_nvs_flash.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

/* On host, esp_log.h is a no-op stub from tests/host_include/. On
 * device, it is the real IDF header. The mock just wants to log
 * diagnostics when keys are missing — not load-bearing for tests. */

static const char *TAG = "mock_nvs";

namespace {

enum class EntryType : uint8_t {
    kU8,
    kStr,
};

struct Entry {
    EntryType type;
    std::vector<uint8_t> bytes;  // raw payload, no length prefix
};

struct Namespace {
    std::map<std::string, Entry> entries;
};

// Global state. Reset by mock_nvs_reset().
std::map<std::string, Namespace> &store() {
    static std::map<std::string, Namespace> instance;
    return instance;
}

// Mock handle table. We never reuse IDs.
uint32_t next_handle = 1;
std::map<nvs_handle_t, std::string> &handle_table() {
    static std::map<nvs_handle_t, std::string> instance;
    return instance;
}

}  // namespace

/* ---------- public mock API (test helpers) ---------- */

extern "C" void mock_nvs_reset(void) {
    store().clear();
    handle_table().clear();
    next_handle = 1;
}

extern "C" void mock_nvs_seed_str(const char *namespace_name,
                                  const char *key,
                                  const char *value) {
    if (!namespace_name || !key || !value) {
        return;
    }
    Entry e;
    e.type = EntryType::kStr;
    e.bytes.assign(value, value + std::strlen(value) + 1);  // incl. NUL
    store()[namespace_name].entries[key] = std::move(e);
}

extern "C" void mock_nvs_seed_u8(const char *namespace_name,
                                 const char *key,
                                 uint8_t value) {
    if (!namespace_name || !key) {
        return;
    }
    Entry e;
    e.type = EntryType::kU8;
    e.bytes.assign(1, value);
    store()[namespace_name].entries[key] = std::move(e);
}

extern "C" esp_err_t mock_nvs_read_u8(const char *namespace_name,
                                      const char *key,
                                      uint8_t *out_value) {
    if (!namespace_name || !key || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    auto ns_it = store().find(namespace_name);
    if (ns_it == store().end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    auto key_it = ns_it->second.entries.find(key);
    if (key_it == ns_it->second.entries.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (key_it->second.type != EntryType::kU8 ||
        key_it->second.bytes.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = key_it->second.bytes[0];
    return ESP_OK;
}

/* ---------- public mock API (NVS drop-in) ---------- */

extern "C" esp_err_t mock_nvs_open(const char *namespace_name,
                                   nvs_open_mode_t /*open_mode*/,
                                   nvs_handle_t *out_handle) {
    if (!namespace_name || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    // Touch the namespace so it exists in the map even if empty.
    store()[namespace_name];
    nvs_handle_t h = next_handle++;
    handle_table()[h] = namespace_name;
    *out_handle = h;
    return ESP_OK;
}

extern "C" void mock_nvs_close(nvs_handle_t handle) {
    handle_table().erase(handle);
}

extern "C" esp_err_t mock_nvs_set_str(nvs_handle_t handle,
                                      const char *key,
                                      const char *value) {
    auto it = handle_table().find(handle);
    if (it == handle_table().end()) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    Entry e;
    e.type = EntryType::kStr;
    e.bytes.assign(value, value + std::strlen(value) + 1);  // incl. NUL
    store()[it->second].entries[key] = std::move(e);
    return ESP_OK;
}

extern "C" esp_err_t mock_nvs_get_str(nvs_handle_t handle,
                                      const char *key,
                                      char *out_value,
                                      size_t *length) {
    auto it = handle_table().find(handle);
    if (it == handle_table().end()) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!key || !length) {
        return ESP_ERR_INVALID_ARG;
    }
    auto ns_it = store().find(it->second);
    if (ns_it == store().end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    auto key_it = ns_it->second.entries.find(key);
    if (key_it == ns_it->second.entries.end() ||
        key_it->second.type != EntryType::kStr) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const auto &bytes = key_it->second.bytes;
    size_t required = bytes.size();  // includes NUL
    if (*length < required) {
        *length = required;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (out_value) {
        std::memcpy(out_value, bytes.data(), required);
    }
    *length = required;
    return ESP_OK;
}

extern "C" esp_err_t mock_nvs_set_u8(nvs_handle_t handle,
                                     const char *key,
                                     uint8_t value) {
    auto it = handle_table().find(handle);
    if (it == handle_table().end()) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    Entry e;
    e.type = EntryType::kU8;
    e.bytes.assign(1, value);
    store()[it->second].entries[key] = std::move(e);
    return ESP_OK;
}

extern "C" esp_err_t mock_nvs_get_u8(nvs_handle_t handle,
                                     const char *key,
                                     uint8_t *out_value) {
    auto it = handle_table().find(handle);
    if (it == handle_table().end()) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!key || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    auto ns_it = store().find(it->second);
    if (ns_it == store().end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    auto key_it = ns_it->second.entries.find(key);
    if (key_it == ns_it->second.entries.end() ||
        key_it->second.type != EntryType::kU8) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_value = key_it->second.bytes[0];
    return ESP_OK;
}

extern "C" esp_err_t mock_nvs_commit(nvs_handle_t /*handle*/) {
    // Mock storage is process memory; commit is a no-op.
    return ESP_OK;
}

extern "C" esp_err_t mock_nvs_erase_key(nvs_handle_t handle,
                                        const char *key) {
    auto it = handle_table().find(handle);
    if (it == handle_table().end()) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    auto ns_it = store().find(it->second);
    if (ns_it == store().end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    auto erased = ns_it->second.entries.erase(key);
    return erased > 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

extern "C" esp_err_t mock_nvs_erase_all(nvs_handle_t handle) {
    auto it = handle_table().find(handle);
    if (it == handle_table().end()) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    auto ns_it = store().find(it->second);
    if (ns_it == store().end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    ns_it->second.entries.clear();
    return ESP_OK;
}

/* ---------- nvs_flash_* / nvs_get_stats mocks ---------- */

extern "C" esp_err_t mock_nvs_flash_init(void) {
    // No-op; mock storage is process memory.
    return ESP_OK;
}

extern "C" esp_err_t mock_nvs_flash_erase(void) {
    // Wipe every namespace in the mock.
    store().clear();
    return ESP_OK;
}

extern "C" esp_err_t mock_nvs_get_stats(const char *partition_name,
                                        nvs_stats_t *out_stats) {
    if (!out_stats) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)partition_name;  // mock ignores partition name

    // Approximate NVS entry accounting: every stored entry ~ 32 B
    // header + the payload size. Sum across every namespace.
    size_t used_entries = 0;
    for (const auto& [name, ns] : store()) {
        (void)name;
        for (const auto& [key, entry] : ns.entries) {
            (void)key;
            // Round up payload to 32-B chunks and add 1 for the
            // entry header. The mock approximates NVS-flash geometry.
            size_t chunks = (entry.bytes.size() + 31) / 32;
            if (chunks == 0) {
                chunks = 1;
            }
            used_entries += 1 + chunks;  // header + chunks
        }
    }

    out_stats->used_entries = static_cast<uint32_t>(used_entries);
    out_stats->free_entries = 0;   // mock never reports free space
    out_stats->total_entries = static_cast<uint32_t>(used_entries);
    out_stats->namespace_count = static_cast<uint32_t>(store().size());
    return ESP_OK;
}