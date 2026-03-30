/*
 * Anykernel OS — Software Watchdog Implementation
 */

#include "watchdog.h"
#include "task.h"
#include "ktimer.h"
#include "kprintf.h"
#include "log.h"
#include "compiler.h"

void watchdog_init(void) {
    LOG_OK("Watchdog initialized (threshold: %u ticks = %u ms)",
           WATCHDOG_THRESHOLD, WATCHDOG_THRESHOLD * TIMER_TICK_MS);
}

__hot void watchdog_check(void) {
    struct task *t = task_current();
    if (unlikely(!t)) return;

    if (unlikely(t->watchdog_ticks >= WATCHDOG_THRESHOLD)) {
        LOG_WARN("WATCHDOG: task '%s' (TID %u) stuck for %u ticks (%u ms)",
                 t->name, t->tid, t->watchdog_ticks,
                 t->watchdog_ticks * TIMER_TICK_MS);
        /* Reset to avoid flooding logs — only warn once per threshold */
        t->watchdog_ticks = 0;
    }
}
