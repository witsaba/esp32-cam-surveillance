/* mock_esp_netif.h — host-side mock for the IDF netif driver.
 *
 * The softAP bring-up calls `esp_netif_create_default_wifi_ap()` to
 * allocate the AP netif and `esp_netif_destroy_default_netif()` to
 * tear it down (the latter from softap_stop()). On host the mock
 * returns a sentinel non-NULL handle by default; tests that want
 * to exercise the NULL-return failure path call
 * `mock_esp_netif_create_default_wifi_ap_return_set(NULL)`.
 */
#pragma once

#include "esp_err.h"

/* Forward-declared to match IDF's `typedef struct esp_netif_obj *esp_netif_t`.
 * On host we just use `void *` — the production code only passes the
 * handle back to esp_netif_destroy() which discards it. */
typedef void *esp_netif_handle_t;
typedef void *esp_netif_t;

/* ---------- primable return values ---------- */
void mock_esp_netif_init_return_set(esp_err_t r);
void mock_esp_netif_create_default_wifi_ap_return_set(esp_netif_handle_t h);
void mock_esp_netif_set_default_netif_return_set(esp_err_t r);
void mock_esp_netif_destroy_return_set(esp_err_t r);

/* ---------- call counters ---------- */
int  mock_esp_netif_init_call_count(void);
int  mock_esp_netif_create_default_wifi_ap_call_count(void);
int  mock_esp_netif_set_default_netif_call_count(void);
int  mock_esp_netif_destroy_call_count(void);

/* ---------- reset ---------- */
void mock_esp_netif_reset(void);

/* ---------- mock targets ---------- */
esp_err_t          mock_esp_netif_init(void);
esp_netif_handle_t mock_esp_netif_create_default_wifi_ap(void);
esp_err_t          mock_esp_netif_set_default_netif(esp_netif_t *netif);
esp_err_t          mock_esp_netif_destroy(esp_netif_t *netif);
