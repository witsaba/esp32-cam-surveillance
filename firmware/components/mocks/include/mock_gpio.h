/* mock_gpio.h — host-side mock for the IDF GPIO driver.
 *
 * FW-06 status-LED exercises the IDF GPIO API:
 *   gpio_config(&cfg)         (one-shot at led_init())
 *   gpio_set_level(pin, lvl)  (on every state transition)
 *
 * On host, the production source (led.c) includes
 * `mock_gpio_link.h` which `#define`s each production symbol
 * to the mock symbol below. The mock returns whatever the
 * test primed via `set_*_return_set()` (default ESP_OK) and
 * records call counts + last pin/level for assertions.
 *
 * On device builds, `MOCK_GPIO_USE_REAL` is defined (or the
 * link header is not included at all) and the real gpio_*
 * functions from IDF are linked.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Forward-declared types matching IDF's gpio_types.h. On host
 * we don't pull in IDF; tests don't construct these types
 * directly. The mock_esp_wifi pattern uses `int` typedefs for
 * the enums; we do the same here so led.c compiles unchanged
 * on host. */
typedef int gpio_num_t;
typedef int gpio_mode_t;
typedef int gpio_int_type_t;
typedef int gpio_pullup_t;
typedef int gpio_pulldown_t;

/* GPIO_NUM_4 — the AI-THINKER onboard LED pin per PRD § FR-7
 * L222. Mirrors the IDF soc/gpio_num.h enum value. */
#ifndef GPIO_NUM_4
#define GPIO_NUM_4 4
#endif

/* gpio_mode_t values from gpio_types.h. */
#ifndef GPIO_MODE_OUTPUT
#define GPIO_MODE_OUTPUT 1
#endif

#ifndef GPIO_INTR_DISABLE
#define GPIO_INTR_DISABLE 0
#endif

#ifndef GPIO_PULLUP_DISABLE
#define GPIO_PULLUP_DISABLE 0
#endif
#ifndef GPIO_PULLDOWN_DISABLE
#define GPIO_PULLDOWN_DISABLE 0
#endif

/* gpio_config_t — opaque stub for host. The mock's
 * gpio_config() does NOT inspect pin_bit_mask/mode/intr_type;
 * it only records the call count. Production source passes a
 * const `gpio_config_t *`. */
typedef struct {
    uint64_t pin_bit_mask;
    int      mode;
    int      pull_up_en;
    int      pull_down_en;
    int      intr_type;
} gpio_config_t;

/* ---------- primable return values (test helpers) ---------- */
void mock_gpio_config_set_return(esp_err_t r);
void mock_gpio_set_level_set_return(esp_err_t r);

/* ---------- call counters ---------- */
int mock_gpio_config_call_count(void);
int mock_gpio_set_level_call_count(void);

/* ---------- captured state ---------- */
void mock_gpio_set_level_capture(int *out_pin, int *out_level);

/* ---------- reset ---------- */
void mock_gpio_reset(void);

/* ---------- mock targets (link-header redirects) ---------- */
esp_err_t mock_gpio_config(const gpio_config_t *cfg);
esp_err_t mock_gpio_set_level(gpio_num_t pin, uint32_t level);
