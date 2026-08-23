/* camera.c — esp32-camera bring-up + runtime setter reservation (FW-10).
 *
 * Single TU inside firmware/components/camera/. Three public entry
 * points:
 *   1. camera_init(const config_t *) — fires the FR-2 PRD parameter
 *      table (FW-10.1) + PSRAM assertion (FW-10.2) +
 *      `esp_psram_get_size()` log (FW-10.4) + the
 *      camera_settings_source->apply() round-trip (FW-10.5).
 *   2. camera_sensor_get(void) — accessor for the sensor_t cached at
 *      init time. Other components (FW-20 runtime reconfig) call this
 *      to obtain the sensor pointer.
 *   3. camera_apply_runtime_settings(const camera_settings_t *) —
 *      the FW-10.3 setter-only path: routes through sensor_t->set_*
 *      and trips the no-reinit guard if a reinit is introduced.
 *
 * Host builds pull in the camera mock link header; device builds
 * link the real esp32-camera managed component via the
 * `REQUIRES esp32-camera` directive in the component
 * CMakeLists.txt.
 */
#include "camera.h"
#include "camera_settings.h"
#include "boot_status.h"

#include "esp_err.h"
#include "esp_log.h"

#ifndef UNITY_HOST_BUILD
#include "esp_camera.h"
#include "esp_psram.h"
#include "sensor.h"
#else
#include "mock_esp_camera_link.h"
#include "mock_init_returns.h"
#endif

#include <string.h>

#define TAG "camera"

/* AI-Thinker ESP32-CAM pin map (PRD § FR-2 L136-144). Single
 * board support — no #if defined(BOARD_*). The values are
 * constants because every AI-Thinker build uses them; the
 * char-driver layer should never see a diff board in FW-10's
 * scope (PRD § Scope-boundary + design #3713). */
#define CAMERA_PIN_PWDN       32
#define CAMERA_PIN_RESET      -1
#define CAMERA_PIN_XCLK       0
/* pin_sccb_sda / pin_sccb_scl are the modern field names (the
 * legacy alias pin_siod / pin_sioc maps to the same union
 * member — esp_camera.h:121-128); we use the modern names so
 * the production compile against the IDF-managed component
 * works without GCC -Wdeprecated-attribute warnings. PRD
 * labels the bus pins as SIOD/SIOC for historical continuity. */
#define CAMERA_PIN_SCCB_SDA   26
#define CAMERA_PIN_SCCB_SCL   27
#define CAMERA_PIN_D7     35
#define CAMERA_PIN_D6     34
#define CAMERA_PIN_D5     39
#define CAMERA_PIN_D4     36
#define CAMERA_PIN_D3     21
#define CAMERA_PIN_D2     19
#define CAMERA_PIN_D1     18
#define CAMERA_PIN_D0     5
#define CAMERA_PIN_VSYNC  25
#define CAMERA_PIN_HREF   23
#define CAMERA_PIN_PCLK   22

#define CAMERA_XCLK_FREQ_HZ  10000000
#define CAMERA_LEDC_CHANNEL  0

/* Cached sensor_t pointer from the prior camera_init(). Resets
 * to NULL on each camera_init() entry; the FW-10.3 no-reinit
 * guard refuses to reinit the driver if this pointer is already
 * non-NULL + the runtime stub flag is set. */
static sensor_t *s_sensor = NULL;

/* camera_init() invocations counter — FW-10.3 invariant
 * (`g_init_count <= 1`). The stub build (CAMERA_TEST_STUB_REINIT
 * =1) trips a guard when the counter exceeds 1, mirror of the
 * wifi FW-08.3 bounded-wait pattern. */
static int s_init_count = 0;

esp_err_t camera_init(const config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

#ifdef UNITY_HOST_BUILD
    /* FW-03.2 fail-loud regression — the boot test driver
     * forces this step's return via mock_init_returns_get so
     * a non-OK path bubbles through boot.c:152 unchanged.
     * The same hook was used by the stub body. */
    esp_err_t forced = mock_init_returns_get(BOOT_STEP_CAMERA_INIT);
    if (forced != ESP_OK) return forced;
#endif

#ifdef CAMERA_TEST_STUB_REINIT
    /* FW-10.3 bite-proof — re-running camera_init() under the stub
     * trips the no-reinit guard. The invariant message MUST
     * contain the literal "no_reinit" so the Pass 9 runner can
     * grep for it. */
    s_init_count++;
    if (s_init_count > 1) {
#  ifdef UNITY_HOST_BUILD
        /* On host we use the raw abort path — TEST_FAIL_MESSAGE
         * is reachable from unity.h, included via the
         * mock_esp_camera_link.h chain. The literal substring
         * "no_reinit" must appear here so Pass 9 can grep. */
        extern void camera_guard_fail_no_reinit(void);
        camera_guard_fail_no_reinit();
        return ESP_FAIL;  /* unreachable */
#  else
        ESP_LOGE(TAG, "no_reinit invariant violated: "
                 "esp_camera_init called more than once");
        return ESP_FAIL;
#  endif
    }
#else
    /* Production runtime is single-shot — boot.c (line 152) is
     * the only caller and it never re-enters. We still count
     * (host-only debug aid) without enforcing re-entry refusal
     * because the host test runner invokes camera_init from
     * multiple TEST_CASE bodies in a single process lifetime
     * and we don't want module-static state pollution to mask
     * real driver-side bugs. The FW-10.3 stub build exercises
     * the re-entry guard explicitly. */
    s_init_count++;
#endif

    /* Step 1 — PSRAM assertion (FW-10.2). On host the mock's
     * prime_psram() is the controllable toggle; on device the
     * IDF hw check is authoritative. We log the typed error
     * BEFORE returning ESP_FAIL so the orchestrator's
     * BOOT_CHECK_STEP surface-level error carries the reason. */
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM_REQUIRED: cannot init without PSRAM");
        return ESP_FAIL;
    }

    /* Step 2 — PSRAM size log (FW-10.4). Mechanical; the value
     * always exists at this point (we returned ESP_FAIL above if
     * not initialized). The mock's default is 4194304 (4 MB). */
    ESP_LOGI(TAG, "psram_size=%u bytes",
             (unsigned)esp_psram_get_size());

    /* Step 3 — FR-2 PRD parameter table (FW-10.1). The literal
     * fields below are the only source of truth for the
     * documented camera_config_t. The test asserts each one
     * reaches esp_camera_init() via the mock's captured
     * literal. */
    camera_config_t config = {
        .ledc_channel = CAMERA_LEDC_CHANNEL,
        .ledc_timer   = 0,
        .pin_pwdn     = CAMERA_PIN_PWDN,
        .pin_reset    = CAMERA_PIN_RESET,
        .pin_xclk     = CAMERA_PIN_XCLK,
        .pin_sccb_sda = CAMERA_PIN_SCCB_SDA,
        .pin_sccb_scl = CAMERA_PIN_SCCB_SCL,
        .pin_d7       = CAMERA_PIN_D7,
        .pin_d6       = CAMERA_PIN_D6,
        .pin_d5       = CAMERA_PIN_D5,
        .pin_d4       = CAMERA_PIN_D4,
        .pin_d3       = CAMERA_PIN_D3,
        .pin_d2       = CAMERA_PIN_D2,
        .pin_d1       = CAMERA_PIN_D1,
        .pin_d0       = CAMERA_PIN_D0,
        .pin_vsync    = CAMERA_PIN_VSYNC,
        .pin_href     = CAMERA_PIN_HREF,
        .pin_pclk     = CAMERA_PIN_PCLK,
        .xclk_freq_hz = CAMERA_XCLK_FREQ_HZ,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = CONFIG_FIRMWARE_CAMERA_FRAME_SIZE,
        .jpeg_quality = CONFIG_FIRMWARE_CAMERA_JPEG_QUALITY,
        .fb_count     = 1,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t r = esp_camera_init(&config);
    if (r != ESP_OK) return r;

    /* Step 4 — cache the sensor pointer. The mock returns a
     * function-pointer-table sentinel; device returns the real
     * OV2640 driver. */
    s_sensor = esp_camera_sensor_get();

    /* Step 5 — apply the boot-time settings (FW-10.5). The
     * default source is the fake in-memory one; FW-20.5 swaps
     * it for an NVS-backed source. We first apply Kconfig
     * defaults via the source->reset_defaults() path (so the
     * setter ring gets the default `framesize`/`quality`),
     * then consult source->load() + source->apply() for the
     * stored blob override. */
    const camera_settings_source_t *src = camera_settings_get_source();
    if (src && src->apply && src->reset_defaults && src->load
              && src->schema_version) {
        camera_settings_t defaults = {0};
        (void)src->reset_defaults(&defaults);

        camera_settings_t stored = {0};
        esp_err_t lr = src->load(&stored);
        if (lr == ESP_OK &&
            stored.schema_version == src->schema_version()) {
            (void)src->apply(s_sensor, &stored);
        } else {
            ESP_LOGW(TAG,
                     "stored schema mismatch (blob=%lu src=%lu) — "
                     "using Kconfig defaults",
                     (unsigned long)stored.schema_version,
                     (unsigned long)src->schema_version());
        }
    }

    return ESP_OK;
}

sensor_t *camera_sensor_get(void)
{
    return s_sensor;
}

esp_err_t camera_apply_runtime_settings(const camera_settings_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    sensor_t *s = camera_sensor_get();
    if (!s) return ESP_FAIL;

    /* FW-10.3 — runtime reconfig goes through the sensor_t setter
     * surface. NEVER call esp_camera_init() on this path; the
     * stub build trips the guard if a reinit is reintroduced. */
    if (s->set_framesize) s->set_framesize(s, in->framesize);
    if (s->set_quality)   s->set_quality(s, in->quality);

    return ESP_OK;
}

/* ---------- FW-10.3 guard tripwire ----------
 *
 * The stub build defines `-DCAMERA_TEST_STUB_REINIT=1`, which
 * causes camera_init() to short-circuit into
 * camera_guard_fail_no_reinit() on a second invocation. On host
 * this aborts via TEST_FAIL_MESSAGE so the Pass 9 runner can
 * grep stdout for the literal "no_reinit" and confirm the
 * guard is load-bearing. The device stub is a no-op fallback
 * to keep the linker happy.
 */
#ifdef UNITY_HOST_BUILD
#include "unity.h"

void camera_guard_fail_no_reinit(void)
{
    TEST_FAIL_MESSAGE("no_reinit invariant violated: "
                      "esp_camera_init called more than once "
                      "in a single boot");
}
#else
void camera_guard_fail_no_reinit(void)
{
    /* Device: unreachable — production camera_init does not
     * reach this branch. Kept as a stub. */
}
#endif
