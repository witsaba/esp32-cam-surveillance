/* mock_esp_timer.c — implementation of the esp_timer mocks.
 *
 * Maintains a static registry of (handle → {callback, arg})
 * so mock_esp_timer_fire_callback() can look up and invoke the
 * registered callback synchronously. The registry is bounded;
 * up to 8 handles can coexist (well above the 2 used by led.c).
 */
#include "mock_esp_timer.h"

#include <string.h>

#define MOCK_ESP_TIMER_MAX_HANDLES 8

typedef struct {
    esp_timer_handle_t handle;
    esp_timer_cb_t     callback;
    void              *arg;
    int                active;  /* 1 = slot in use, 0 = free */
} mock_esp_timer_slot_t;

/* Opaque handle struct — must match the typedef in the .h so
 * tests can compare pointers. */
struct mock_esp_timer_handle {
    int slot_index;  /* index into the slot table */
};

static mock_esp_timer_slot_t g_slots[MOCK_ESP_TIMER_MAX_HANDLES];

static esp_err_t g_create_return         = ESP_OK;
static esp_err_t g_start_periodic_return = ESP_OK;
static esp_err_t g_start_once_return     = ESP_OK;
static esp_err_t g_stop_return           = ESP_OK;
static esp_err_t g_restart_return        = ESP_OK;
static esp_err_t g_delete_return         = ESP_OK;

static int g_create_count         = 0;
static int g_start_periodic_count = 0;
static int g_start_once_count     = 0;
static int g_stop_count           = 0;
static int g_restart_count        = 0;
static int g_delete_count         = 0;

static uint64_t g_last_period_us         = 0;
static uint64_t g_last_period_us_oneshot = 0;

void mock_esp_timer_create_set_return(esp_err_t r)         { g_create_return         = r; }
void mock_esp_timer_start_periodic_set_return(esp_err_t r) { g_start_periodic_return = r; }
void mock_esp_timer_start_once_set_return(esp_err_t r)     { g_start_once_return     = r; }
void mock_esp_timer_stop_set_return(esp_err_t r)           { g_stop_return           = r; }
void mock_esp_timer_restart_set_return(esp_err_t r)        { g_restart_return        = r; }
void mock_esp_timer_delete_set_return(esp_err_t r)         { g_delete_return         = r; }

int mock_esp_timer_create_call_count(void)         { return g_create_count; }
int mock_esp_timer_start_periodic_call_count(void) { return g_start_periodic_count; }
int mock_esp_timer_start_once_call_count(void)     { return g_start_once_count; }
int mock_esp_timer_stop_call_count(void)           { return g_stop_count; }
int mock_esp_timer_restart_call_count(void)        { return g_restart_count; }
int mock_esp_timer_delete_call_count(void)         { return g_delete_count; }

uint64_t mock_esp_timer_last_period_us(void)         { return g_last_period_us; }
uint64_t mock_esp_timer_last_period_us_oneshot(void) { return g_last_period_us_oneshot; }

int mock_esp_timer_handle_count(void) { return g_create_count; }

esp_timer_handle_t mock_esp_timer_handle_at(int idx)
{
    if (idx < 0 || idx >= MOCK_ESP_TIMER_MAX_HANDLES) return NULL;
    if (!g_slots[idx].active) return NULL;
    return g_slots[idx].handle;
}

/* Find a slot by handle pointer. Returns -1 if not found. */
static int find_slot(esp_timer_handle_t h)
{
    if (!h) return -1;
    for (int i = 0; i < MOCK_ESP_TIMER_MAX_HANDLES; ++i) {
        if (g_slots[i].active && g_slots[i].handle == h) return i;
    }
    return -1;
}

void mock_esp_timer_reset(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    g_create_return         = ESP_OK;
    g_start_periodic_return = ESP_OK;
    g_start_once_return     = ESP_OK;
    g_stop_return           = ESP_OK;
    g_restart_return        = ESP_OK;
    g_delete_return         = ESP_OK;
    g_create_count          = 0;
    g_start_periodic_count  = 0;
    g_start_once_count      = 0;
    g_stop_count            = 0;
    g_restart_count         = 0;
    g_delete_count          = 0;
    g_last_period_us        = 0;
    g_last_period_us_oneshot = 0;
}

esp_err_t mock_esp_timer_create(const esp_timer_create_args_t *args,
                                  esp_timer_handle_t *out_handle)
{
    g_create_count++;
    if (g_create_return != ESP_OK) return g_create_return;
    if (!args || !args->callback || !out_handle) return ESP_ERR_INVALID_ARG;

    /* Allocate a free slot. */
    int slot_idx = -1;
    for (int i = 0; i < MOCK_ESP_TIMER_MAX_HANDLES; ++i) {
        if (!g_slots[i].active) { slot_idx = i; break; }
    }
    if (slot_idx < 0) return ESP_ERR_NO_MEM;

    /* Backing storage for the handle struct. We embed it in the
     * slot to keep the allocation lifetime tied to the slot. The
     * handle pointer is what the test receives back. */
    static struct mock_esp_timer_handle backing[MOCK_ESP_TIMER_MAX_HANDLES];
    backing[slot_idx].slot_index = slot_idx;

    g_slots[slot_idx].handle   = &backing[slot_idx];
    g_slots[slot_idx].callback = args->callback;
    g_slots[slot_idx].arg      = args->arg;
    g_slots[slot_idx].active   = 1;

    *out_handle = g_slots[slot_idx].handle;
    return ESP_OK;
}

esp_err_t mock_esp_timer_start_periodic(esp_timer_handle_t handle,
                                          uint64_t period_us)
{
    g_start_periodic_count++;
    g_last_period_us = period_us;
    (void)handle;
    return g_start_periodic_return;
}

esp_err_t mock_esp_timer_start_once(esp_timer_handle_t handle,
                                      uint64_t timeout_us)
{
    g_start_once_count++;
    g_last_period_us_oneshot = timeout_us;
    (void)handle;
    return g_start_once_return;
}

esp_err_t mock_esp_timer_stop(esp_timer_handle_t handle)
{
    g_stop_count++;
    (void)handle;
    return g_stop_return;
}

esp_err_t mock_esp_timer_restart(esp_timer_handle_t handle,
                                    uint64_t timeout_us)
{
    g_restart_count++;
    g_last_period_us = timeout_us;
    (void)handle;
    return g_restart_return;
}

esp_err_t mock_esp_timer_delete(esp_timer_handle_t handle)
{
    g_delete_count++;
    int idx = find_slot(handle);
    if (idx >= 0) {
        g_slots[idx].active   = 0;
        g_slots[idx].callback = NULL;
        g_slots[idx].arg      = NULL;
        g_slots[idx].handle   = NULL;
    }
    return g_delete_return;
}

esp_err_t mock_esp_timer_fire_callback(esp_timer_handle_t handle)
{
    int idx = find_slot(handle);
    if (idx < 0 || !g_slots[idx].callback) return ESP_ERR_NOT_FOUND;
    g_slots[idx].callback(g_slots[idx].arg);
    return ESP_OK;
}
