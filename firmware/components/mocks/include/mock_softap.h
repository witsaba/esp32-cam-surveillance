/* mock_softap.h — host-side mock for the softap component (FW-08).
 *
 * Mirrors mock_esp_wifi.c shape: primable return values + call
 * counters. Production softap_stop() is wrapped via the mock_softap
 * link header; tests drive `mock_softap_stop_call_count()` to
 * assert the IP-up handler fired softap_stop exactly once
 * (FW-08.4).
 *
 * `mock_softap_is_active_set_return(bool)` primes the
 * softap_is_active() getter — FW-08.5 asserts the wifi component
 * selects WIFI_MODE_APSTA when this returns true. The link-header
 * redirect routes the production getter to the mock's getter
 * function.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* ---------- primable return values ---------- */
void mock_softap_stop_return_set(esp_err_t r);
void mock_softap_is_active_set_return(bool active);

/* ---------- call counters ---------- */
int  mock_softap_stop_call_count(void);

/* ---------- reset ---------- */
void mock_softap_reset(void);

/* ---------- mock targets (called via the link-header redirect) ---------- */
esp_err_t mock_softap_stop(void);
/* Mock getter — production reads g_cfg_valid (softap.c); the link-
 * header redirect on host swaps it for this function. */
bool mock_softap_is_active_get(void);
