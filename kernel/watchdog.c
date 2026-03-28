/*
 * Anykernel OS — Software Watchdog Implementation
 */

#include "watchdog.h"
#include "task.h"
#include "kprintf.h"
#include "log.h"
#include "compiler.h"

static uint32_t watchdog_warnings = 0;

void watchdog_init(void) {
    watchdog_warnings = 0;
    LOG_OK("Watchdog initialized (threshold: %u ticks = %u ms)",
           WATCHDOG_THRESHOLD, WATCHDOG_THRESHOLD * 10);
}

__hot void watchdog_check(void) {
    struct task *t = task_current();
    if (unlikely(!t)) return;

    if (unlikely(t->watchdog_ticks >= WATCHDOG_THRESHOLD)) {
        watchdog_warnings++;
        LOG_WARN("WATCHDOG: task '%s' (TID %u) stuck for %u ticks (%u ms)",
                 t->name, t->tid, t->watchdog_ticks,
                 t->watchdog_ticks * 10);
        /* Reset to avoid flooding logs — only warn once per threshold */
        t->watchdog_ticks = 0;
    }
}
