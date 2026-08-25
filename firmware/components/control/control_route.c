/* control_route.c — FW-18 PURE router core: strict-cJSON parse +
 * six-command allow-list + registry seam + unified error-envelope
 * builder. Zero IDF/ws/httpd/mock includes — host suites exercise
 * this TU with no mock linkage (spec #3970 "pure-core isolation").
 *
 * Classification taxonomy (ruling #3966.3):
 *   total parse failure          → bad_json   (id ONLY if recoverable,
 *                                               tier-2 salvage below)
 *   valid JSON, unusable cmd     → unknown    (+ id-if-present)
 *   allow-listed, no handler     → not_implemented (retires at FW-21)
 *   allow-listed WITH handler    → dispatch (handler gets the VALIDATED
 *                                   body; no envelope emitted here)
 *
 * Id echo (rulings #3966.4/.8 + D9): the ORIGINAL cJSON item travels
 * borrowed through control_parsed_t until reply composition, so the
 * reply echoes value AND type losslessly (42 stays 42). Object/array/
 * null ids are treated as absent → member omitted. String ids longer
 * than CONTROL_ID_ECHO_MAX are omitted entirely (never truncated to a
 * lie, never null).
 */
#include "control.h"

#include <stdio.h>
#include <string.h>

/* ---------- module-static registry ---------- */

static const struct {
    const char      *name;
    control_cmd_id_t id;
} k_allow_list[CONTROL_CMD_COUNT] = {
    { "stream",    CONTROL_CMD_STREAM    },
    { "config",    CONTROL_CMD_CONFIG    },
    { "reset_cam", CONTROL_CMD_RESET_CAM },
    { "sleep",     CONTROL_CMD_SLEEP     },
    { "reboot",    CONTROL_CMD_REBOOT    },
    { "identify",  CONTROL_CMD_IDENTIFY  },
};

static control_handler_fn s_handlers[CONTROL_CMD_COUNT];

void control_handler_register(control_cmd_id_t id, control_handler_fn fn)
{
    if (id >= 0 && id < CONTROL_CMD_COUNT) s_handlers[id] = fn;
}

control_handler_fn control_dispatch(const control_parsed_t *p)
{
    if (!p || p->cmd_id < 0 || p->cmd_id >= CONTROL_CMD_COUNT) return NULL;
    return s_handlers[p->cmd_id];
}

void control_reset_for_test(void)
{
#ifdef UNITY_HOST_BUILD
    memset(s_handlers, 0, sizeof(s_handlers));
#endif
}

/* ---------- id helpers ---------- */

/* Usable scalar per D9: string / number / true / false. */
static bool id_is_usable(const cJSON *id)
{
    return id && (cJSON_IsString(id) || cJSON_IsNumber(id) ||
                  cJSON_IsBool(id));
}

/* Borrow the doc's id item when present + usable. */
static void carry_doc_id(const cJSON *doc, control_parsed_t *out)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(doc, "id");
    if (id_is_usable(id)) {
        out->has_id = true;
        out->id     = id;
    }
}

/* Tier-2 salvage (D2): the body failed strict parse entirely — bound
 * lexical scan for the FIRST `"id"` key, then let cJSON itself parse
 * the single scalar that follows (ParseWithLength returns the first
 * value and tolerates trailing junk — the exact tolerance tier 1
 * leans on). Any lexing violation, non-scalar, or over-cap string →
 * the id is omitted (deterministic, ~30 lines, both branches pinned
 * by test_control_route.c S5/S6/S8). */
static void salvage_id(const char *frame, size_t len, control_parsed_t *out)
{
    static const char k_needle[] = "\"id\"";
    enum { N = (int)(sizeof(k_needle) - 1) };
    const char *end = frame + len;

    for (const char *p = frame; p + N <= end; ++p) {
        if (memcmp(p, k_needle, N) != 0) continue;
        const char *v = p + N;
        while (v < end && (*v == ' ' || *v == '\t' ||
                           *v == '\n' || *v == '\r')) v++;
        if (v >= end || *v != ':') continue;
        v++;
        while (v < end && (*v == ' ' || *v == '\t' ||
                           *v == '\n' || *v == '\r')) v++;

        cJSON *item = cJSON_ParseWithLength(v, (size_t)(end - v));
        if (!item) return;                     /* lexing violation → omit */
        if (!id_is_usable(item)) {             /* object/array/null → absent */
            cJSON_Delete(item);
            return;
        }
        if (cJSON_IsString(item) &&
            strlen(item->valuestring) > CONTROL_ID_ECHO_MAX) {
            cJSON_Delete(item);                /* over-cap → omitted */
            return;
        }
        out->has_id       = true;
        out->id_temp_owned = true;             /* caller frees once */
        out->id           = item;
        return;
    }
}

/* ---------- parse / classify ---------- */

control_parse_status_t control_parse(const char *frame, size_t len,
                                     cJSON **doc_out, control_parsed_t *out)
{
    if (!out) return CONTROL_BAD_JSON;
    if (doc_out) *doc_out = NULL;
    memset(out, 0, sizeof(*out));
    out->status = CONTROL_BAD_JSON;

    if (!frame || len == 0) {
        salvage_id(frame, len, out);
        return CONTROL_BAD_JSON;
    }

    cJSON *doc = cJSON_ParseWithLength(frame, len);
    if (!doc) {                                /* tier 2: total failure */
        salvage_id(frame, len, out);
        return CONTROL_BAD_JSON;
    }
    if (doc_out) *doc_out = doc;

    if (!cJSON_IsObject(doc)) {
        out->status = CONTROL_UNKNOWN_CMD;     /* e.g. `[1,2]` body */
        return out->status;
    }

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(doc, "cmd");
    if (!cJSON_IsString(cmd) || !cmd->valuestring) {
        out->status = CONTROL_UNKNOWN_CMD;
        carry_doc_id(doc, out);
        return out->status;
    }

    for (int i = 0; i < CONTROL_CMD_COUNT; ++i) {
        if (strcmp(cmd->valuestring, k_allow_list[i].name) != 0) continue;
        out->cmd_id = k_allow_list[i].id;
        carry_doc_id(doc, out);
        out->status = s_handlers[i] ? CONTROL_PARSED_DISPATCH
                                    : CONTROL_NOT_IMPLEMENTED;
        return out->status;
    }

    out->status = CONTROL_UNKNOWN_CMD;         /* not on the allow-list */
    carry_doc_id(doc, out);
    return out->status;
}

/* ---------- unified error-envelope builder ---------- */

/* Bounded string-id escaper (D6): " → \" , \ → \\ , bytes <0x20 →
 * \u00XX. `src` is ≤ CONTROL_ID_ECHO_MAX chars (enforced by caller),
 * so dst needs 6*CONTROL_ID_ECHO_MAX+1 bytes worst case (~385). */
static void escape_string_id(const char *src, char *dst)
{
    char *d = dst;
    for (const char *p = src; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"')           { *d++ = '\\'; *d++ = '"'; }
        else if (c == '\\')     { *d++ = '\\'; *d++ = '\\'; }
        else if (c < 0x20)      { snprintf(d, 7, "\\u%04x", c); d += 6; }
        else                    { *d++ = (char)c; }
    }
    *d = '\0';
}

size_t control_error_build(const char *reason_token, const cJSON *id,
                           char *out, size_t out_len)
{
    if (!reason_token || !out || out_len == 0) return 0;

    /* Echo-cap gate FIRST (ruling #3966.8): over-cap or unusable
     * string ids omit the member entirely — never truncated, never
     * null. */
    bool str_ok = cJSON_IsString(id) && id->valuestring &&
                  strlen(id->valuestring) <= CONTROL_ID_ECHO_MAX;

    int n;
    if (!id_is_usable(id)) {
        n = snprintf(out, out_len,
                     "{\"type\":\"error\",\"reason\":\"%s\"}",
                     reason_token);
    } else if (cJSON_IsString(id) && !str_ok) {
        n = snprintf(out, out_len,
                     "{\"type\":\"error\",\"reason\":\"%s\"}",
                     reason_token);
    } else if (cJSON_IsString(id)) {
        char esc[6 * CONTROL_ID_ECHO_MAX + 1];
        escape_string_id(id->valuestring, esc);
        n = snprintf(out, out_len,
                     "{\"type\":\"error\",\"reason\":\"%s\",\"id\":\"%s\"}",
                     reason_token, esc);
    } else if (cJSON_IsNumber(id)) {
        /* cJSON print_number rule: integral doubles render %d. */
        if (id->valuedouble == (double)id->valueint) {
            n = snprintf(out, out_len,
                         "{\"type\":\"error\",\"reason\":\"%s\",\"id\":%d}",
                         reason_token, id->valueint);
        } else {
            n = snprintf(out, out_len,
                         "{\"type\":\"error\",\"reason\":\"%s\",\"id\":%.17g}",
                         reason_token, id->valuedouble);
        }
    } else {
        n = snprintf(out, out_len,
                     "{\"type\":\"error\",\"reason\":\"%s\",\"id\":%s}",
                     reason_token, cJSON_IsTrue(id) ? "true" : "false");
    }

    if (n < 0 || (size_t)n >= out_len) return 0;   /* 0-sentinel: skip send */
    return (size_t)n;
}

/* ---------- process-one-frame pipeline ---------- */

size_t control_frame_process(const char *frame, size_t len,
                             char *out, size_t out_len)
{
    if (!frame || !out || out_len == 0) return 0;

    cJSON *doc = NULL;
    control_parsed_t p;
    control_parse(frame, len, &doc, &p);

    size_t n = 0;
    switch (p.status) {
    case CONTROL_BAD_JSON:
        n = control_error_build("bad_json", p.id, out, out_len);
        break;
    case CONTROL_UNKNOWN_CMD:
        n = control_error_build("unknown", p.id, out, out_len);
        break;
    case CONTROL_NOT_IMPLEMENTED:
        n = control_error_build("not_implemented", p.id, out, out_len);
        break;
    case CONTROL_PARSED_DISPATCH: {
        /* Registry seam: FW-19/20/21 handlers receive the VALIDATED
         * body and own their side effects/replies. Nothing goes on
         * the wire from here. */
        control_handler_fn fn = control_dispatch(&p);
        if (fn) (void)fn(frame, len, NULL);
        n = 0;
        break;
    }
    default:
        n = 0;
        break;
    }

    /* Single-owner free-once: the parse doc OR the salvage temp —
     * never both. */
    if (doc) cJSON_Delete(doc);
    else if (p.id_temp_owned) cJSON_Delete((cJSON *)(uintptr_t)p.id);
    return n;
}
