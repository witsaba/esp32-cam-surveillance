/* mock_init_returns.c — per-step return-value slot implementation. */
#include "mock_init_returns.h"

#include <string.h>

static esp_err_t g_returns[BOOT_STEP_COUNT];

void mock_init_returns_set(boot_step_t step, esp_err_t ret) {
    if ((unsigned)step < BOOT_STEP_COUNT) g_returns[step] = ret;
}

esp_err_t mock_init_returns_get(boot_step_t step) {
    if ((unsigned)step >= BOOT_STEP_COUNT) return ESP_OK;
    return g_returns[step];
}

void mock_init_returns_reset(void) {
    for (size_t i = 0; i < BOOT_STEP_COUNT; ++i) g_returns[i] = ESP_OK;
}