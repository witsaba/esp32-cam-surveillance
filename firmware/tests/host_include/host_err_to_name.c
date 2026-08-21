/* host_err_to_name.c — host implementation of IDF's esp_err_to_name().
 *
 * On host, the production code (boot.c) calls `esp_err_to_name(ret)`
 * to render the error code in fail-loud log lines. IDF's real
 * implementation lives in a generated source file that's not linked
 * into the host build; we provide a minimal stub that returns
 * "ERR_<code>" so the log line carries the integer error code.
 */
#include <stdio.h>

#include "esp_err.h"

const char *esp_err_to_name(esp_err_t code)
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "ERR_%d", (int)code);
    return buf;
}

const char *esp_err_to_name_r(esp_err_t code, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return "";
    snprintf(buf, buflen, "ERR_%d", (int)code);
    return buf;
}