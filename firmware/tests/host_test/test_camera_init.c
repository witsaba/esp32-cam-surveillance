/* test_camera_init.c — FW-10.1 6-row camera_init parameter table.
 *
 * The camera component MUST build a `camera_config_t` literal at
 * camera_init() time that matches PRD § FR-2 (table L136-144):
 *
 *   | row | field          | expected value                     |
 *   | 1   | pixel_format   | PIXFORMAT_JPEG                     |
 *   | 2   | frame_size     | CONFIG_FIRMWARE_CAMERA_FRAME_SIZE  |
 *   |     |               | (default 5 = FRAMESIZE_QVGA)       |
 *   | 3   | jpeg_quality   | CONFIG_FIRMWARE_CAMERA_JPEG_QUALITY|
 *   |     |               | (default 18)                       |
 *   | 4   | fb_count       | 1                                  |
 *   | 5   | grab_mode      | CAMERA_GRAB_WHEN_EMPTY             |
 *   | 6   | xclk_freq_hz   | 10_000_000                         |
 *
 * Plus the AI-THINKER pin map (PRD § Scope-boundary L134-144):
 *   pin_pwdn = 32, pin_reset = -1, pin_xclk = 0,
 *   pin_siod = 26, pin_sioc = 27  (esp32-camera's modern
 *   field names: pin_sccb_sda = 26, pin_sccb_scl = 27),
 *   pin_d7..d0 = {35, 34, 39, 36, 21, 19, 18, 5},
 *   pin_vsync = 25, pin_href = 23, pin_pclk = 22,
 *   ledc_channel = 0.
 *
 * The mock_esp_camera triplet captures the camera_config_t that
 * camera_init() passes to esp_camera_init(); the tests below
 * inspect that capture via mock_esp_camera_last_init_config().
 *
 * Conventions: TEST_ASSERT_EQUAL_INT for enum/int fields (the
 * mock's capture struct uses `int` slots for these). Constants
 * come from the FW-02 Kconfig mirror values; on host the test
 * asserts the literal defaults (18, 5).
 */
#include "camera.h"

#include "config.h"
#include "mock_esp_camera.h"
#include "mock_esp_camera_link.h"
#include "mock_log.h"
#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

/* Standard fixture: reset the mocks + prime PSRAM present + ESP_OK
 * init return. Mirrors the wifi_init_with_mocks() shape. */
static esp_err_t camera_init_with_mocks(void)
{
    mock_esp_camera_reset();
    mock_log_reset();

    /* Prime PSRAM present (4 MB default) + init returns ESP_OK so
     * the FR-2 config reaches esp_camera_init(). */
    mock_esp_camera_prime_psram(true, 4194304);
    mock_esp_camera_init_return_set(ESP_OK);

    config_t cfg = {0};
    return camera_init(&cfg);
}

TEST_CASE(
    "test_fw10_1_pixel_format_is_jpeg [fw-10.1][row-1]",
    "[camera][fw-10.1][params]")
{
    esp_err_t rc = camera_init_with_mocks();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    const camera_config_t *cfg = mock_esp_camera_last_init_config();
    TEST_ASSERT_NOT_NULL(cfg);
    /* PIXFORMAT_JPEG == 4 in esp32-camera v2.1.7. */
    TEST_ASSERT_EQUAL_INT(4 /* PIXFORMAT_JPEG */, cfg->pixel_format);
}

TEST_CASE(
    "test_fw10_1_frame_size_is_qvga_default [fw-10.1][row-2]",
    "[camera][fw-10.1][params]")
{
    esp_err_t rc = camera_init_with_mocks();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    const camera_config_t *cfg = mock_esp_camera_last_init_config();
    TEST_ASSERT_NOT_NULL(cfg);
    /* CONFIG_FIRMWARE_CAMERA_FRAME_SIZE default = 5 (FRAMESIZE_QVGA). */
    TEST_ASSERT_EQUAL_INT(CONFIG_FIRMWARE_CAMERA_FRAME_SIZE, cfg->frame_size);
    TEST_ASSERT_EQUAL_INT(5, cfg->frame_size);
}

TEST_CASE(
    "test_fw10_1_jpeg_quality_default_is_18 [fw-10.1][row-3]",
    "[camera][fw-10.1][params]")
{
    esp_err_t rc = camera_init_with_mocks();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    const camera_config_t *cfg = mock_esp_camera_last_init_config();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL_INT(CONFIG_FIRMWARE_CAMERA_JPEG_QUALITY,
                          cfg->jpeg_quality);
    TEST_ASSERT_EQUAL_INT(18, cfg->jpeg_quality);
}

TEST_CASE(
    "test_fw10_1_fb_count_is_one [fw-10.1][row-4]",
    "[camera][fw-10.1][params]")
{
    esp_err_t rc = camera_init_with_mocks();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    const camera_config_t *cfg = mock_esp_camera_last_init_config();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL_INT(1, cfg->fb_count);
}

TEST_CASE(
    "test_fw10_1_grab_mode_is_when_empty [fw-10.1][row-5]",
    "[camera][fw-10.1][params]")
{
    esp_err_t rc = camera_init_with_mocks();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    const camera_config_t *cfg = mock_esp_camera_last_init_config();
    TEST_ASSERT_NOT_NULL(cfg);
    /* CAMERA_GRAB_WHEN_EMPTY == 0 in esp32-camera v2.1.7
     * (driver/esp_camera.c grabs `mode = 0` when buffer is empty). */
    TEST_ASSERT_EQUAL_INT(0 /* CAMERA_GRAB_WHEN_EMPTY */, cfg->grab_mode);
}

TEST_CASE(
    "test_fw10_1_xclk_freq_is_10mhz [fw-10.1][row-6]",
    "[camera][fw-10.1][params]")
{
    esp_err_t rc = camera_init_with_mocks();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    const camera_config_t *cfg = mock_esp_camera_last_init_config();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL_INT(10000000, cfg->xclk_freq_hz);
}

/* AI-Thinker pin map (PRD § FR-2). Combined with the 6 parameter
 * rows above; this scenario verifies every pin lands in the
 * configured struct. The pin map regression is the highest-impact
 * device-flash failure mode — separate scenario for grep-ability. */
TEST_CASE(
    "test_fw10_1_ai_thinker_pin_map [fw-10.1][pin-map]",
    "[camera][fw-10.1][pin-map]")
{
    esp_err_t rc = camera_init_with_mocks();
    TEST_ASSERT_EQUAL_INT(ESP_OK, rc);

    const camera_config_t *cfg = mock_esp_camera_last_init_config();
    TEST_ASSERT_NOT_NULL(cfg);

    /* Power + clock + I2C bus. */
    TEST_ASSERT_EQUAL_INT(32, cfg->pin_pwdn);
    TEST_ASSERT_EQUAL_INT(-1, cfg->pin_reset);
    TEST_ASSERT_EQUAL_INT(0, cfg->pin_xclk);
    TEST_ASSERT_EQUAL_INT(26, cfg->pin_sccb_sda);
    TEST_ASSERT_EQUAL_INT(27, cfg->pin_sccb_scl);

    /* 8-bit data bus D7..D0 (per PRD § Scope-boundary L141-142). */
    TEST_ASSERT_EQUAL_INT(35, cfg->pin_d7);
    TEST_ASSERT_EQUAL_INT(34, cfg->pin_d6);
    TEST_ASSERT_EQUAL_INT(39, cfg->pin_d5);
    TEST_ASSERT_EQUAL_INT(36, cfg->pin_d4);
    TEST_ASSERT_EQUAL_INT(21, cfg->pin_d3);
    TEST_ASSERT_EQUAL_INT(19, cfg->pin_d2);
    TEST_ASSERT_EQUAL_INT(18, cfg->pin_d1);
    TEST_ASSERT_EQUAL_INT(5,  cfg->pin_d0);

    /* Sync + pixel clock. */
    TEST_ASSERT_EQUAL_INT(25, cfg->pin_vsync);
    TEST_ASSERT_EQUAL_INT(23, cfg->pin_href);
    TEST_ASSERT_EQUAL_INT(22, cfg->pin_pclk);

    /* LEDC channel 0 for XCLK generation (PRD § FR-2). */
    TEST_ASSERT_EQUAL_INT(0, cfg->ledc_channel);
}
