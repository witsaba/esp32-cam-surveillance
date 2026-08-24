/* ws_sink_recorder.c — host-test recorder for the FW-16 viewer
 * sink seam. See ws_sink_recorder.h for the contract. */
#include "ws_sink_recorder.h"

#include <string.h>

#include "ws.h"

#define REC_TEXT_CAP   512
#define REC_TEXT_SLOTS 12
#define REC_BIN_CAP    24576
#define REC_BIN_SLOTS  8

typedef struct {
    size_t  len;
    char    data[REC_TEXT_CAP + 1];
} rec_text_slot_t;

typedef struct {
    size_t  len;
    uint8_t data[REC_BIN_CAP];
} rec_bin_slot_t;

static rec_text_slot_t s_text[REC_TEXT_SLOTS];
static size_t          s_text_count;
static rec_bin_slot_t  s_bin[REC_BIN_SLOTS];
static size_t          s_bin_count;
static bool            s_fail_all;

void ws_sink_recorder_reset(void)
{
    memset(s_text, 0, sizeof(s_text));
    memset(s_bin, 0, sizeof(s_bin));
    s_text_count = 0;
    s_bin_count  = 0;
    s_fail_all   = false;
}

void ws_sink_recorder_fail_all_set(bool fail_all)
{
    s_fail_all = fail_all;
}

size_t ws_sink_recorder_text_count(void) { return s_text_count; }
size_t ws_sink_recorder_bin_count(void)  { return s_bin_count; }

esp_err_t ws_sink_recorder_get_text_at(size_t idx, char *out,
                                        size_t cap)
{
    if (!out || cap == 0 || idx >= s_text_count) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = s_text[idx].len < cap - 1 ? s_text[idx].len : cap - 1;
    memcpy(out, s_text[idx].data, n);
    out[n] = '\0';
    return ESP_OK;
}

esp_err_t ws_sink_recorder_get_bin_at(size_t idx, uint8_t *out,
                                       size_t cap, size_t *len)
{
    if (!out || !len || idx >= s_bin_count) return ESP_ERR_INVALID_ARG;
    size_t n = s_bin[idx].len < cap ? s_bin[idx].len : cap;
    memcpy(out, s_bin[idx].data, n);
    *len = n;
    return ESP_OK;
}

/* ---------- sink vtable ---------- */

static esp_err_t rec_send_bin(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    if (s_fail_all) return ESP_FAIL;

    if (s_bin_count < REC_BIN_SLOTS) {
        rec_bin_slot_t *slot = &s_bin[s_bin_count++];
        slot->len = len < REC_BIN_CAP ? len : REC_BIN_CAP;
        memcpy(slot->data, buf, slot->len);
    } else {
        memmove(s_bin, s_bin + 1,
                (REC_BIN_SLOTS - 1) * sizeof(rec_bin_slot_t));
        rec_bin_slot_t *slot = &s_bin[REC_BIN_SLOTS - 1];
        slot->len = len < REC_BIN_CAP ? len : REC_BIN_CAP;
        memcpy(slot->data, buf, slot->len);
    }
    return ESP_OK;
}

static esp_err_t rec_send_text(const char *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    if (s_fail_all) return ESP_FAIL;

    if (s_text_count < REC_TEXT_SLOTS) {
        rec_text_slot_t *slot = &s_text[s_text_count++];
        slot->len = len < REC_TEXT_CAP ? len : REC_TEXT_CAP;
        memcpy(slot->data, buf, slot->len);
        slot->data[slot->len] = '\0';
    } else {
        memmove(s_text, s_text + 1,
                (REC_TEXT_SLOTS - 1) * sizeof(rec_text_slot_t));
        rec_text_slot_t *slot = &s_text[REC_TEXT_SLOTS - 1];
        slot->len = len < REC_TEXT_CAP ? len : REC_TEXT_CAP;
        memcpy(slot->data, buf, slot->len);
        slot->data[slot->len] = '\0';
    }
    return ESP_OK;
}

static bool rec_is_connected(void)
{
    return !s_fail_all;
}

esp_err_t ws_sink_recorder_install(void)
{
    static const ws_sink_t vt = {
        .send_bin     = rec_send_bin,
        .send_text    = rec_send_text,
        .is_connected = rec_is_connected,
    };
    ws_sink_install(&vt);
    return ESP_OK;
}
