/* boot_priq.h — FreeRTOS task priority + stack constants for the
 * orchestrator-owned tasks (currently just the 4 no-op supervision
 * stubs). Matches PRD § FR-3 task_prio=5 and the WS-client default
 * task_stack. Downstream milestones override per-role as needed.
 */
#pragma once

#define BOOT_TASK_PRIO_DEFAULT         5
#define BOOT_TASK_PRIO_SUPERVISION     5
#define BOOT_TASK_STACK_DEFAULT     4096
#define BOOT_TASK_STACK_SUPERVISION 4096