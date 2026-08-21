/* main.c — ESP-IDF firmware entry point (FW-03).
 *
 * Thin wrapper around the boot orchestrator. The orchestrator owns
 * the FR-1 sequence; `app_main` is now the dispatcher entry-point
 * only. FW-02's NVS init / config_load log lines are now emitted
 * by `boot_run()` itself (the orchestrator owns the FR-1 steps 1–2
 * per PRD § FR-1).
 */
#include "boot.h"

void app_main(void)
{
    (void)boot_run();
}