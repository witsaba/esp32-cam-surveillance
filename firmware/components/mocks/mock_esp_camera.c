/* mock_esp_camera.c — implementation of the esp32-camera + PSRAM mocks.
 *
 * Mirrors mock_esp_wifi.c shape. Tests prime the camera_init()
 * return + the PSRAM presence flag via set_* helpers; each
 * camera_config_t literal is captured so the FW-10.1 row tests
 * can assert every PRD-mandated parameter.
 *
 * The sensor_t sentinel is a static instance with a function-
 * pointer table that records each set_* call's argument in a
 * ring buffer (newest-first lookup via sensor_set_arg_at). The
 * FW-10.5 walking-skeleton test asserts that Kconfig defaults are
 * applied first + the stored blob overrides via set_quality
 * (ring idx=0 is the most recent call).
 */
#include "mock_esp_camera.h"

#include <string.h>

static esp_err_t g_init_return   = ESP_OK;
static esp_err_t g_deinit_return = ESP_OK;
static int       g_init_count    = 0;
static int       g_deinit_count  = 0;

static bool      g_psram_present = true;
static size_t    g_psram_size    = 4194304; /* 4 MB */

static camera_config_t g_last_config;
static int             g_last_captured = 0;

/* ---------- sensor_t sentinel + setter ring buffer ---------- */

static sensor_t g_sensor_sentinel;

/* Setter ring buffers — newest-first lookup. Capacity MOCK_CAMERA_
 * SET_RING_CAP (8). Out-of-range queries return -1. */
static int g_set_quality_args  [MOCK_CAMERA_SET_RING_CAP];
static int g_set_framesize_args[MOCK_CAMERA_SET_RING_CAP];
static int g_set_vflip_args    [MOCK_CAMERA_SET_RING_CAP];
static int g_set_hmirror_args  [MOCK_CAMERA_SET_RING_CAP];
static int g_set_pixformat_args[MOCK_CAMERA_SET_RING_CAP];
static int g_set_grab_mode_args[MOCK_CAMERA_SET_RING_CAP];
static size_t g_set_head = 0;

static void record_set_int(int *ring, int value)
{
    if (g_set_head < MOCK_CAMERA_SET_RING_CAP) {
        ring[g_set_head++] = value;
    } else {
        memmove(ring, ring + 1, (MOCK_CAMERA_SET_RING_CAP - 1) * sizeof(int));
        ring[MOCK_CAMERA_SET_RING_CAP - 1] = value;
    }
}

static int ring_arg_at(const int *ring, int idx)
{
    if (idx < 0 || (size_t)idx >= g_set_head) return -1;
    return ring[g_set_head - 1 - (size_t)idx];
}

static int mock_init_status(sensor_t *s)        { (void)s; return 0; }
static int mock_reset(sensor_t *s)              { (void)s; return 0; }

static int mock_set_vflip(sensor_t *s, int v)
{
    (void)s;
    record_set_int(g_set_vflip_args, v);
    return 0;
}

static int mock_set_hmirror(sensor_t *s, int v)
{
    (void)s;
    record_set_int(g_set_hmirror_args, v);
    return 0;
}

static int mock_set_quality(sensor_t *s, int q)
{
    (void)s;
    record_set_int(g_set_quality_args, q);
    return 0;
}

static int mock_set_framesize(sensor_t *s, int f)
{
    (void)s;
    record_set_int(g_set_framesize_args, f);
    return 0;
}

static int mock_set_pixformat(sensor_t *s, int p)
{
    (void)s;
    record_set_int(g_set_pixformat_args, p);
    return 0;
}

static int mock_set_grab_mode(sensor_t *s, int m)
{
    /* grab_mode is not on the real sensor_t — we add a mock-only
     * slot for the host test that walks the setter surface. The
     * production build doesn't reference set_grab_mode (the IDF
     * driver sets it internally); on host the mock provides it so
     * production source that wraps through can record args. */
    (void)s;
    record_set_int(g_set_grab_mode_args, m);
    return 0;
}

static int mock_set_contrast(sensor_t *s, int v)  { (void)s; return 0; }
static int mock_set_brightness(sensor_t *s, int v){ (void)s; return 0; }
static int mock_set_saturation(sensor_t *s, int v){ (void)s; return 0; }
static int mock_set_sharpness(sensor_t *s, int v) { (void)s; return 0; }
static int mock_set_denoise(sensor_t *s, int v)   { (void)s; return 0; }
static int mock_set_gainceiling(sensor_t *s, int v){ (void)s; return 0; }
static int mock_set_colorbar(sensor_t *s, int v)  { (void)s; return 0; }
static int mock_set_whitebal(sensor_t *s, int v)  { (void)s; return 0; }
static int mock_set_gain_ctrl(sensor_t *s, int v) { (void)s; return 0; }
static int mock_set_exposure_ctrl(sensor_t *s, int v){ (void)s; return 0; }

/* ---------- public mock API ---------- */

void mock_esp_camera_init_return_set(esp_err_t r)        { g_init_return = r; }
void mock_esp_camera_deinit_return_set(esp_err_t r)      { g_deinit_return = r; }
void mock_esp_camera_prime_psram(bool present, size_t sz)
{
    g_psram_present = present;
    g_psram_size    = sz;
}

int mock_esp_camera_init_call_count(void)   { return g_init_count; }
int mock_esp_camera_deinit_call_count(void) { return g_deinit_count; }

const camera_config_t *mock_esp_camera_last_init_config(void)
{
    return g_last_captured ? &g_last_config : NULL;
}

sensor_t *mock_esp_camera_sensor_get_sentinel(void)
{
    return &g_sensor_sentinel;
}

int mock_esp_camera_sensor_set_quality_arg_at  (int idx) { return ring_arg_at(g_set_quality_args,   idx); }
int mock_esp_camera_sensor_set_framesize_arg_at(int idx) { return ring_arg_at(g_set_framesize_args, idx); }
int mock_esp_camera_sensor_set_vflip_arg_at    (int idx) { return ring_arg_at(g_set_vflip_args,     idx); }
int mock_esp_camera_sensor_set_hmirror_arg_at  (int idx) { return ring_arg_at(g_set_hmirror_args,   idx); }
int mock_esp_camera_sensor_set_pixformat_arg_at(int idx) { return ring_arg_at(g_set_pixformat_args, idx); }
int mock_esp_camera_sensor_set_grab_mode_arg_at(int idx) { return ring_arg_at(g_set_grab_mode_args, idx); }

void mock_esp_camera_reset(void)
{
    g_init_return    = ESP_OK;
    g_deinit_return  = ESP_OK;
    g_init_count     = 0;
    g_deinit_count   = 0;
    g_psram_present  = true;
    g_psram_size     = 4194304;
    memset(&g_last_config, 0, sizeof(g_last_config));
    g_last_captured = 0;

    /* Wire the sensor sentinel's setter slots. We initialize ALL
     * the slots FW-10 uses (mirroring the real esp32-camera
     * sensor_t layout) so production source compiled unchanged
     * chains through the mock. The host-only set_grab_mode is
     * a mock extension; on device the IDF driver sets grab_mode
     * internally during esp_camera_init(). */
    g_sensor_sentinel.init_status       = mock_init_status;
    g_sensor_sentinel.reset             = mock_reset;
    g_sensor_sentinel.set_pixformat     = mock_set_pixformat;
    g_sensor_sentinel.set_framesize     = mock_set_framesize;
    g_sensor_sentinel.set_contrast      = mock_set_contrast;
    g_sensor_sentinel.set_brightness    = mock_set_brightness;
    g_sensor_sentinel.set_saturation    = mock_set_saturation;
    g_sensor_sentinel.set_sharpness     = mock_set_sharpness;
    g_sensor_sentinel.set_denoise       = mock_set_denoise;
    g_sensor_sentinel.set_gainceiling   = mock_set_gainceiling;
    g_sensor_sentinel.set_quality       = mock_set_quality;
    g_sensor_sentinel.set_colorbar      = mock_set_colorbar;
    g_sensor_sentinel.set_whitebal      = mock_set_whitebal;
    g_sensor_sentinel.set_gain_ctrl     = mock_set_gain_ctrl;
    g_sensor_sentinel.set_exposure_ctrl = mock_set_exposure_ctrl;
    g_sensor_sentinel.set_hmirror       = mock_set_hmirror;
    g_sensor_sentinel.set_vflip         = mock_set_vflip;

    memset(g_set_quality_args,   0, sizeof(g_set_quality_args));
    memset(g_set_framesize_args, 0, sizeof(g_set_framesize_args));
    memset(g_set_vflip_args,     0, sizeof(g_set_vflip_args));
    memset(g_set_hmirror_args,   0, sizeof(g_set_hmirror_args));
    memset(g_set_pixformat_args, 0, sizeof(g_set_pixformat_args));
    memset(g_set_grab_mode_args, 0, sizeof(g_set_grab_mode_args));
    g_set_head = 0;
}

/* ---------- mock targets (called via the link-header redirect) ---------- */

esp_err_t mock_esp_camera_init(const camera_config_t *config)
{
    g_init_count++;
    if (config) {
        g_last_config    = *config;
        g_last_captured  = 1;
    }
    return g_init_return;
}

esp_err_t mock_esp_camera_deinit(void)
{
    g_deinit_count++;
    return g_deinit_return;
}

sensor_t *mock_esp_camera_sensor_get(void)
{
    return &g_sensor_sentinel;
}

bool mock_esp_psram_is_initialized(void)
{
    return g_psram_present;
}

size_t mock_esp_psram_get_size(void)
{
    return g_psram_size;
}
