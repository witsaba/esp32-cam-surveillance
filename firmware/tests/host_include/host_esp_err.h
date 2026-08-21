/* host_esp_err.h — host stub of IDF's esp_err.h.
 *
 * Provides just the esp_err_t enum + error code constants that our
 * mocks reference. The real esp_err.h on IDF is part of the esp_common
 * component and pulls in many more headers (soc_caps.h, etc.). This
 * stub lets the host build proceed with a plain gcc.
 *
 * ESP_OK is defined as 0 to match IDF's convention.
 */
#ifndef HOST_ESP_ERR_H
#define HOST_ESP_ERR_H

typedef int esp_err_t;

/* Generic ESP-IDF error codes referenced by FW-02. */
#define ESP_OK                          0
#define ESP_ERR_INVALID_ARG              0x102
#define ESP_ERR_NVS_BASE                 0x1300
#define ESP_ERR_NVS_NOT_FOUND            (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_INVALID_HANDLE       (ESP_ERR_NVS_BASE + 0x0F)
#define ESP_ERR_NVS_INVALID_LENGTH       (ESP_ERR_NVS_BASE + 0x0C)

#endif /* HOST_ESP_ERR_H */