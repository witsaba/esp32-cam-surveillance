/* test_identity_mac_hex.c — FW-13 identity_mac_to_hex_lower tests.
 *
 * Three tests cover the pure hex-encode surface:
 *   1. test_mac_hex_format: a non-trivial MAC -> expected 12-char
 *      lowercase hex without separators (the canonical AI-Thinker
 *      ESP32-CAM MAC c8:f0:9e:9d:50:08 -> "c8f09e9d5008").
 *   2. test_mac_hex_zero: all-zero MAC -> "000000000000" (the
 *      factory-default / unprogrammed eFuse case; tests that the
 *      helper handles the all-zero input correctly).
 *   3. test_mac_hex_overflow: out_len=12 (one less than required)
 *      returns ESP_ERR_INVALID_ARG (defensive cap enforcement).
 *
 * These three tests bring Pass 1 to 108 (was 105 from T-13-B).
 *
 * Convention: assertions use uint8_t arrays + char buffers sized
 * to the contract (out_len >= 13). No IDF mocks needed — the
 * helper is pure. Mirrors test_wifi_backoff.c's pure-helper
 * test shape (FW-08.1 charter L742-748 schedule tests).
 */
#include "identity.h"

#include <string.h>

#include "unity.h"

#ifdef UNITY_HOST_BUILD
#include "unity_host_test_runner.h"
#else
#include "unity_test_runner.h"
#endif

TEST_CASE(
    "test_mac_hex_format [fw-13][identity][mac-hex]",
    "[identity][fw-13][mac-hex]")
{
    /* AI-Thinker ESP32-CAM factory-programmed MAC (canonical
     * sample from charter L1196). */
    const uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0x9d, 0x50, 0x08};
    char out[13] = {0};
    esp_err_t r = identity_mac_to_hex_lower(mac, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(ESP_OK, r);
    TEST_ASSERT_EQUAL_STRING("c8f09e9d5008", out);
}

TEST_CASE(
    "test_mac_hex_zero [fw-13][identity][mac-hex][boundary]",
    "[identity][fw-13][mac-hex]")
{
    /* All-zero MAC = factory-default / unprogrammed eFuse case. */
    const uint8_t mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    char out[13] = {0};
    esp_err_t r = identity_mac_to_hex_lower(mac, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(ESP_OK, r);
    TEST_ASSERT_EQUAL_STRING("000000000000", out);
}

TEST_CASE(
    "test_mac_hex_overflow [fw-13][identity][mac-hex][overflow]",
    "[identity][fw-13][mac-hex]")
{
    /* out_len=12 is one less than required (13 = 12 chars + NUL).
     * The contract says ESP_ERR_INVALID_ARG; the buffer must be
     * left untouched (no partial write). */
    const uint8_t mac[6] = {0xc8, 0xf0, 0x9e, 0x9d, 0x50, 0x08};
    char out[13];
    memset(out, 0xAA, sizeof(out));  /* poison to detect partial write */
    esp_err_t r = identity_mac_to_hex_lower(mac, out, 12);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, r);
    /* The contract is "buffer left untouched" on ESP_ERR_INVALID_ARG
     * — assert the first byte is still the poison value. */
    TEST_ASSERT_EQUAL_HEX8(0xAA, (uint8_t)out[0]);
}