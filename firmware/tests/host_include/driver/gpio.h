/* driver/gpio.h — host stub of IDF's driver/gpio.h (FW-06 host tests).
 *
 * On host, the production source (led.c) calls gpio_config and
 * gpio_set_level. Each call site is redirected to the mock via
 * mock_gpio_link.h BEFORE this header is included. The mock
 * implements the function; we do NOT re-declare prototypes here —
 * the link-header redirect would turn them into conflicting mock_*
 * prototypes.
 *
 * The constants (GPIO_NUM_4, GPIO_MODE_OUTPUT, GPIO_INTR_DISABLE,
 * GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE) and the typedefs
 * (gpio_num_t, gpio_config_t, gpio_mode_t, etc.) live in
 * mock_gpio.h and are visible when this header is included AFTER
 * mock_gpio_link.h. This file exists primarily to satisfy any
 * `#include <driver/gpio.h>` line in led.c.
 */
#ifndef HOST_DRIVER_GPIO_H
#define HOST_DRIVER_GPIO_H

/* No-op: the real declarations live in mock_gpio.h via the
 * link-header redirect. We include mock_gpio.h to pull in the
 * constants + typedefs so led.c compiles unchanged on host. */
#include "mock_gpio.h"

#endif /* HOST_DRIVER_GPIO_H */
