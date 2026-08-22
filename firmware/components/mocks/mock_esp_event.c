/* mock_esp_event.c — implementation of the event loop mocks.
 *
 * FW-08 adds a capture-table for esp_event_handler_instance_register_with
 * + a fire_handler test entry that synchronously invokes the captured
 * handler with the same arity as IDF's esp_event_handler_t. Mirrors
 * mock_esp_timer_fire_callback at mock_esp_timer.c:205-211.
 */
#include "mock_esp_event.h"

#include <string.h>

#define MOCK_ESP_EVENT_MAX_CAPTURES 8

typedef struct {
    esp_event_base_t             base;
    int32_t                      id;
    esp_event_handler_t          handler;
    void                        *arg;
    esp_event_handler_instance_t instance;
    int                          in_use;
} mock_esp_event_capture_t;

static mock_esp_event_capture_t g_captures[MOCK_ESP_EVENT_MAX_CAPTURES];

static esp_err_t g_create_return                = ESP_OK;
static esp_err_t g_register_return               = ESP_OK;

static int g_create_count                       = 0;
static int g_register_count                     = 0;
static int g_fire_handler_count                 = 0;

void mock_esp_event_loop_create_default_return_set(esp_err_t r)
{
    g_create_return = r;
}

void mock_esp_event_handler_instance_register_return_set(esp_err_t r)
{
    g_register_return = r;
}

int mock_esp_event_loop_create_default_call_count(void)
{
    return g_create_count;
}

int mock_esp_event_handler_instance_register_call_count(void)
{
    return g_register_count;
}

int mock_esp_event_fire_handler_call_count(void)
{
    return g_fire_handler_count;
}

void mock_esp_event_reset(void)
{
    memset(g_captures, 0, sizeof(g_captures));
    g_create_return        = ESP_OK;
    g_register_return      = ESP_OK;
    g_create_count         = 0;
    g_register_count       = 0;
    g_fire_handler_count   = 0;
}

int mock_esp_event_registered_count(void)
{
    int n = 0;
    for (int i = 0; i < MOCK_ESP_EVENT_MAX_CAPTURES; ++i) {
        if (g_captures[i].in_use) n++;
    }
    return n;
}

void mock_esp_event_last_captured_handler(esp_event_handler_t *out_handler,
                                            void **out_arg)
{
    /* Find the most recent capture. */
    for (int i = MOCK_ESP_EVENT_MAX_CAPTURES - 1; i >= 0; --i) {
        if (g_captures[i].in_use) {
            if (out_handler) *out_handler = g_captures[i].handler;
            if (out_arg)      *out_arg      = g_captures[i].arg;
            return;
        }
    }
    if (out_handler) *out_handler = NULL;
    if (out_arg)      *out_arg      = NULL;
}

void mock_esp_event_last_captured_base_id(esp_event_base_t *out_base,
                                            int32_t *out_id)
{
    for (int i = MOCK_ESP_EVENT_MAX_CAPTURES - 1; i >= 0; --i) {
        if (g_captures[i].in_use) {
            if (out_base) *out_base = g_captures[i].base;
            if (out_id)   *out_id   = g_captures[i].id;
            return;
        }
    }
    if (out_base) *out_base = NULL;
    if (out_id)   *out_id   = -1;
}

esp_err_t mock_esp_event_loop_create_default(void)
{
    g_create_count++;
    return g_create_return;
}

esp_err_t mock_esp_event_handler_instance_register(
    esp_event_base_t base,
    int32_t event_id,
    esp_event_handler_t handler,
    void *arg,
    esp_event_handler_instance_t *instance)
{
    g_register_count++;
    if (g_register_return != ESP_OK) return g_register_return;
    if (!handler) return ESP_ERR_INVALID_ARG;

    /* Allocate a free slot. */
    int slot_idx = -1;
    for (int i = 0; i < MOCK_ESP_EVENT_MAX_CAPTURES; ++i) {
        if (!g_captures[i].in_use) { slot_idx = i; break; }
    }
    if (slot_idx < 0) return ESP_ERR_NO_MEM;

    /* Back the instance with a static sentinel — callers receive
     * a non-NULL pointer they can later pass to unregister. */
    static char g_sentinel_instances[MOCK_ESP_EVENT_MAX_CAPTURES];
    g_captures[slot_idx].base     = base;
    g_captures[slot_idx].id       = event_id;
    g_captures[slot_idx].handler  = handler;
    g_captures[slot_idx].arg      = arg;
    g_captures[slot_idx].instance = &g_sentinel_instances[slot_idx];
    g_captures[slot_idx].in_use   = 1;

    if (instance) *instance = g_captures[slot_idx].instance;
    return ESP_OK;
}

esp_err_t mock_esp_event_fire_handler(esp_event_base_t base,
                                       int32_t event_id,
                                       void *event_data)
{
    g_fire_handler_count++;
    /* Linear scan for first matching (base, id). */
    for (int i = 0; i < MOCK_ESP_EVENT_MAX_CAPTURES; ++i) {
        if (g_captures[i].in_use &&
            g_captures[i].base == base &&
            g_captures[i].id == event_id &&
            g_captures[i].handler) {
            g_captures[i].handler(g_captures[i].arg,
                                   base, event_id, event_data);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
