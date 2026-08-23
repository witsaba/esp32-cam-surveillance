/* ws_reconnects.c — WS reconnect counter stub (FW-13, T-13-C).
 *
 * Returns 0. FW-14 owns the real producer and replaces this
 * body without changing the public header (ws_reconnects.h).
 * The status-frame builder reads this field; FW-13 asserts it
 * is == 0 in the T-13-I RED test (test_ws_status_payload.c ::
 * test_reconnects_zero_in_fw13).
 */
#include "ws_reconnects.h"

uint32_t ws_reconnects_get(void)
{
    /* FW-14: replace with the real reconnect counter producer.
     * Charter L1201: "FW-14 owns the reconnect counter producer;
     * FW-13.6 status payload reads it; zero until FW-14." */
    return 0u;
}