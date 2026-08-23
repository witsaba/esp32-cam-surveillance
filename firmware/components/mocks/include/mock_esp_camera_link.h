/* mock_esp_camera_link.h — macro-redirect for the esp32-camera API.
 *
 * Mirrors mock_esp_wifi_link.h. On host (UNITY_HOST_BUILD defined)
 * every esp_camera_* / esp_psram_* call site is replaced by the
 * mock_* symbol below. Production source includes this BEFORE
 * any esp32-camera header.
 *
 * On device, the macros are inactive and the real IDF symbols are
 * linked via the esp32-camera managed-component dep declared in
 * firmware/main/idf_component.yml.
 */
#pragma once

#include "mock_esp_camera.h"

#ifndef MOCK_CAMERA_USE_REAL

#define esp_camera_init(cfg)               mock_esp_camera_init(cfg)
#define esp_camera_deinit()                mock_esp_camera_deinit()
#define esp_camera_sensor_get()            mock_esp_camera_sensor_get()
#define esp_psram_is_initialized()         mock_esp_psram_is_initialized()
#define esp_psram_get_size()               mock_esp_psram_get_size()

#endif  /* !MOCK_CAMERA_USE_REAL */
