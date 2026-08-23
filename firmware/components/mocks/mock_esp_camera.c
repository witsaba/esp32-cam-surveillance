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

/* ---------- FW-11 frame-buffer + heap-caps mock state ---------- */

/* Static camera_fb_t instance. `mock_esp_camera_fb_get()` returns
 * its address; `mock_esp_camera_fb_return()` clears the slot's
 * `buf` pointer so subsequent calls return the same struct again
 * (mirrors the real driver's fb_count=1 ownership semantics
 * where only ONE buffer is in-flight at a time). */
static camera_fb_t  g_fb;
static int          g_fb_get_count   = 0;
static bool         g_fb_return_flag = false;
static size_t       g_fb_size        = 11520; /* QVGA JPEG default */

/* heap_caps_get_free_size() mock state — primable per-cap. The
 * FW-11.5 closing check asserts PSRAM decreases by `g_fb_size`
 * (11520 bytes) after the first fb_get, mimicking the real
 * allocator's heap_caps_malloc(MALLOC_CAP_SPIRAM) path. */
static uint32_t     g_caps_free_spiram   = 4000000;
static uint32_t     g_caps_free_internal = 200000;
static int          g_fb_alloc_count     = 0; /* increments per fb_get */

/* ---------- sensor_t sentinel + setter ring buffer ---------- */

static sensor_t g_sensor_sentinel;

/* Setter ring buffers — newest-first lookup. Each ring has
 * its OWN head pointer so the six setters can be interleaved
 * without polluting one another's lookup; out-of-range queries
 * return -1. Capacity MOCK_CAMERA_SET_RING_CAP (8) per ring. */
static int g_set_quality_args  [MOCK_CAMERA_SET_RING_CAP];
static int g_set_framesize_args[MOCK_CAMERA_SET_RING_CAP];
static int g_set_vflip_args    [MOCK_CAMERA_SET_RING_CAP];
static int g_set_hmirror_args  [MOCK_CAMERA_SET_RING_CAP];
static int g_set_pixformat_args[MOCK_CAMERA_SET_RING_CAP];
static int g_set_grab_mode_args[MOCK_CAMERA_SET_RING_CAP];

static size_t g_set_quality_head   = 0;
static size_t g_set_framesize_head = 0;
static size_t g_set_vflip_head     = 0;
static size_t g_set_hmirror_head   = 0;
static size_t g_set_pixformat_head = 0;
static size_t g_set_grab_mode_head = 0;

static void record_set_int(int *ring, size_t *head, int value)
{
    if (*head < MOCK_CAMERA_SET_RING_CAP) {
        ring[(*head)++] = value;
    } else {
        memmove(ring, ring + 1, (MOCK_CAMERA_SET_RING_CAP - 1) * sizeof(int));
        ring[MOCK_CAMERA_SET_RING_CAP - 1] = value;
    }
}

static int ring_arg_at(const int *ring, size_t head, int idx)
{
    if (idx < 0 || (size_t)idx >= head) return -1;
    return ring[head - 1 - (size_t)idx];
}

static int mock_init_status(sensor_t *s)        { (void)s; return 0; }
static int mock_reset(sensor_t *s)              { (void)s; return 0; }

static int mock_set_vflip(sensor_t *s, int v)
{
    (void)s;
    record_set_int(g_set_vflip_args, &g_set_vflip_head, v);
    return 0;
}

static int mock_set_hmirror(sensor_t *s, int v)
{
    (void)s;
    record_set_int(g_set_hmirror_args, &g_set_hmirror_head, v);
    return 0;
}

static int mock_set_quality(sensor_t *s, int q)
{
    (void)s;
    record_set_int(g_set_quality_args, &g_set_quality_head, q);
    return 0;
}

static int mock_set_framesize(sensor_t *s, int f)
{
    (void)s;
    record_set_int(g_set_framesize_args, &g_set_framesize_head, f);
    return 0;
}

static int mock_set_pixformat(sensor_t *s, int p)
{
    (void)s;
    record_set_int(g_set_pixformat_args, &g_set_pixformat_head, p);
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
    record_set_int(g_set_grab_mode_args, &g_set_grab_mode_head, m);
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

int mock_esp_camera_sensor_set_quality_arg_at  (int idx) { return ring_arg_at(g_set_quality_args,   g_set_quality_head,   idx); }
int mock_esp_camera_sensor_set_framesize_arg_at(int idx) { return ring_arg_at(g_set_framesize_args, g_set_framesize_head, idx); }
int mock_esp_camera_sensor_set_vflip_arg_at    (int idx) { return ring_arg_at(g_set_vflip_args,     g_set_vflip_head,     idx); }
int mock_esp_camera_sensor_set_hmirror_arg_at  (int idx) { return ring_arg_at(g_set_hmirror_args,   g_set_hmirror_head,   idx); }
int mock_esp_camera_sensor_set_pixformat_arg_at(int idx) { return ring_arg_at(g_set_pixformat_args, g_set_pixformat_head, idx); }
int mock_esp_camera_sensor_set_grab_mode_arg_at(int idx) { return ring_arg_at(g_set_grab_mode_args, g_set_grab_mode_head, idx); }

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

    /* Reset FW-11 frame-buffer + heap-caps mock state. */
    memset(&g_fb, 0, sizeof(g_fb));
    g_fb_get_count     = 0;
    g_fb_return_flag   = false;
    g_fb_size          = 11520;
    g_caps_free_spiram   = 4000000;
    g_caps_free_internal = 200000;
    g_fb_alloc_count     = 0;

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
    g_set_quality_head   = 0;
    g_set_framesize_head = 0;
    g_set_vflip_head     = 0;
    g_set_hmirror_head   = 0;
    g_set_pixformat_head = 0;
    g_set_grab_mode_head = 0;
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

/* ---------- FW-11 frame-buffer + heap-caps mock targets ---------- */

int  mock_esp_camera_fb_get_call_count(void)   { return g_fb_get_count; }
bool mock_esp_camera_fb_return_was_called(void) { return g_fb_return_flag; }
void mock_esp_camera_fb_size_set(size_t len)    { g_fb_size = len; }
void mock_esp_camera_heap_caps_set(uint32_t spiram, uint32_t internal)
{
    g_caps_free_spiram   = spiram;
    g_caps_free_internal = internal;
}

camera_fb_t *mock_esp_camera_fb_get(void)
{
    g_fb_get_count++;
    g_fb_alloc_count++;
    /* Populate the static fb each call — buf is non-NULL to
     * signal "got a frame", len = g_fb_size (default 11520). */
    g_fb.buf     = (uint8_t *)0x1000; /* sentinel non-NULL */
    g_fb.len     = g_fb_size;
    g_fb.width   = 320;
    g_fb.height  = 240;
    g_fb.format  = PIXFORMAT_JPEG;
    g_fb.fb_count = 1;
    /* Each successful fb_get decreases PSRAM free size by the
     * frame-buffer allocation. Mirrors the FW-10 device-verify
     * log "cam_hal: Allocating 11520 Byte frame buffer in
     * PSRAM". */
    if (g_caps_free_spiram >= g_fb_size) {
        g_caps_free_spiram -= g_fb_size;
    }
    return &g_fb;
}

void mock_esp_camera_fb_return(camera_fb_t *fb)
{
    (void)fb;
    g_fb_return_flag = true;
    /* The real driver would return the buffer to its free
     * pool; on host we just clear the buf slot so the next
     * fb_get() returns the same struct again (matches
     * fb_count=1 semantics). */
    g_fb.buf = NULL;
}

size_t mock_esp_camera_heap_caps_get_free_size(uint32_t caps)
{
    if (caps & MALLOC_CAP_SPIRAM)   return g_caps_free_spiram;
    if (caps & MALLOC_CAP_INTERNAL) return g_caps_free_internal;
    return 0;
}
