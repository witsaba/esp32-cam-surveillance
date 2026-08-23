/* mock_esp_camera.h — host-side mock for the esp32-camera driver.
 *
 * FW-10 mirrors mock_esp_wifi.{h,c,link.h} shape:
 *   - primable return values via set_*_return_set() helpers
 *   - call counters + captured state (camera_config_t literal +
 *     sensor_t setter args via ring buffer)
 *   - reset() to clear all mocks between tests
 *   - host-only — device builds link the real esp_camera_* from IDF
 *     via the managed-component dep in firmware/main/idf_component.yml
 *
 * The mock targets `esp_camera_init`, `esp_camera_deinit`,
 * `esp_camera_sensor_get`, `esp_camera_fb_get`, `esp_camera_fb_return`,
 * `esp_psram_is_initialized`, `esp_psram_get_size`, and the
 * sensor_t setter callbacks (set_vflip, set_hmirror, set_quality,
 * set_framesize, set_pixformat, set_grab_mode). Sensor setters
 * record their argument in a ring buffer so FW-10.5 walking-
 * skeleton tests can verify the order of Kconfig-then-stored
 * setter calls.
 *
 * The mock triplet mirrors mock_esp_wifi; the link-header
 * (mock_esp_camera_link.h) does the macro-redirect.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* sensor_t — mirror of esp32-camera's full struct. The real
 * esp32-camera header defines sensor_t as a typedef with function
 * pointer slots directly embedded (set_pixformat, set_framesize,
 * set_quality, set_hmirror, set_vflip, ... — ~25 setters).
 *
 * On host we replicate the public surface that FW-10.5 walking-
 * skeleton tests inspect: set_quality, set_framesize, set_vflip,
 * set_hmirror, set_pixformat, set_grab_mode. The mock's function
 * pointers chain through `s->set_quality(s, n)` so production
 * source compiles unchanged; each mock setter records its
 * argument in a ring buffer (newest-first lookup).
 *
 * Other fields the real sensor_t has (id, slv_addr, pixformat, ... )
 * are opaque to FW-10 — the host mock fills them with zeros. The
 * mock's struct layout intentionally matches the production one
 * for the slots FW-10 uses (FW-20 will add the 23 OV2640 setters
 * when NVS persistence lands; no FW-10 change required). */
typedef struct _sensor sensor_t;

struct _sensor {
    int id;
    int slv_addr;
    int pixformat;
    int status;
    int xclk_freq_hz;

    /* Function-pointer slots — mirror the real esp32-camera
     * struct verbatim (FW-10.5 walking-skeleton asserts on the
     * setters' arg order: Kconfig defaults first, then stored
     * overrides). */
    int  (*init_status)         (sensor_t *sensor);
    int  (*reset)               (sensor_t *sensor);
    int  (*set_pixformat)       (sensor_t *sensor, int pixformat);
    int  (*set_framesize)       (sensor_t *sensor, int framesize);
    int  (*set_contrast)        (sensor_t *sensor, int level);
    int  (*set_brightness)      (sensor_t *sensor, int level);
    int  (*set_saturation)      (sensor_t *sensor, int level);
    int  (*set_sharpness)       (sensor_t *sensor, int level);
    int  (*set_denoise)         (sensor_t *sensor, int level);
    int  (*set_gainceiling)     (sensor_t *sensor, int gainceiling);
    int  (*set_quality)         (sensor_t *sensor, int quality);
    int  (*set_colorbar)        (sensor_t *sensor, int enable);
    int  (*set_whitebal)        (sensor_t *sensor, int enable);
    int  (*set_gain_ctrl)       (sensor_t *sensor, int enable);
    int  (*set_exposure_ctrl)   (sensor_t *sensor, int enable);
    int  (*set_hmirror)         (sensor_t *sensor, int enable);
    int  (*set_vflip)           (sensor_t *sensor, int enable);
};

/* camera_config_t — full struct matching esp32-camera v2.1.7's
 * camera_config_t (managed_components/espressif__esp32-camera
 * driver/include/esp_camera.h:117-158). We declare it here
 * (host-only) so production source compiles unchanged. The mock's
 * esp_camera_init() captures the literal so FW-10.1 can assert
 * every PRD-mandated parameter row.
 *
 * Note: the real struct uses `pin_sccb_sda`/`pin_sccb_scl` (the
 * modern names, sitting inside a union with the deprecated
 * `pin_siod`/`pin_sioc` aliases). We replicate the modern field
 * names — the union shape is collapsed to two ints since the
 * FW-10.1 tests only assert one of the two aliases. */
typedef struct {
    int       pin_pwdn;
    int       pin_reset;
    int       pin_xclk;
    int       pin_sccb_sda;   /* union in the real struct; pin_siod alias */
    int       pin_sccb_scl;   /* union in the real struct; pin_sioc alias */
    int       pin_d7;
    int       pin_d6;
    int       pin_d5;
    int       pin_d4;
    int       pin_d3;
    int       pin_d2;
    int       pin_d1;
    int       pin_d0;
    int       pin_vsync;
    int       pin_href;
    int       pin_pclk;
    int       xclk_freq_hz;
    int       ledc_timer;
    int       ledc_channel;
    int       pixel_format;
    int       frame_size;
    int       jpeg_quality;
    int       fb_count;
    int       fb_location;
    int       grab_mode;
    int       sccb_i2c_port;
} camera_config_t;

/* PIXFORMAT_JPEG == 4 + CAMERA_GRAB_WHEN_EMPTY == 0 — keep
 * locally so test files don't pull in real esp_camera.h. */
#ifndef PIXFORMAT_JPEG
#define PIXFORMAT_JPEG 4
#endif
#ifndef CAMERA_GRAB_WHEN_EMPTY
#define CAMERA_GRAB_WHEN_EMPTY 0
#endif

/* ---------- primable return values ---------- */
void mock_esp_camera_init_return_set(esp_err_t r);
void mock_esp_camera_deinit_return_set(esp_err_t r);
/* Prime the PSRAM presence flag. The FW-10.2 leaf flips this to
 * (false, 0) to exercise the PSRAM_REQUIRED typed-error path. */
void mock_esp_camera_prime_psram(bool present, size_t size_bytes);

/* ---------- call counters / captured state ---------- */
int  mock_esp_camera_init_call_count(void);
int  mock_esp_camera_deinit_call_count(void);
/* Return the captured config (NULL if esp_camera_init has not
 * been called). The pointer is valid until mock_esp_camera_reset
 * is called. */
const camera_config_t *mock_esp_camera_last_init_config(void);
/* sensor_t * returned by mock_esp_camera_sensor_get (sentinel). */
sensor_t *mock_esp_camera_sensor_get_sentinel(void);

/* sensor_t setter ring buffers (newest-first). Capacity 8 covers
 * a typical boot (Kconfig defaults + NVS overrides). Out-of-range
 * returns -1. */
#define MOCK_CAMERA_SET_RING_CAP 8
int mock_esp_camera_sensor_set_quality_arg_at(int idx);
int mock_esp_camera_sensor_set_framesize_arg_at(int idx);
int mock_esp_camera_sensor_set_vflip_arg_at(int idx);
int mock_esp_camera_sensor_set_hmirror_arg_at(int idx);
int mock_esp_camera_sensor_set_pixformat_arg_at(int idx);
int mock_esp_camera_sensor_set_grab_mode_arg_at(int idx);

/* ---------- reset ---------- */
void mock_esp_camera_reset(void);

/* ---------- mock targets ---------- */
esp_err_t mock_esp_camera_init(const camera_config_t *config);
esp_err_t mock_esp_camera_deinit(void);
sensor_t *mock_esp_camera_sensor_get(void);
/* PSRAM presence + size getters. The FW-10.4 leaf asserts
 * `esp_psram_get_size() == 4194304` and FW-10.2 flips
 * `esp_psram_is_initialized()` to false. */
bool      mock_esp_psram_is_initialized(void);
size_t    mock_esp_psram_get_size(void);

#ifdef __cplusplus
}
#endif
