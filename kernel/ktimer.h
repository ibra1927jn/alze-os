/*
 * Anykernel OS — Kernel Timer Wheel
 *
 * Hierarchical timer system for scheduling deferred callbacks.
 * Like Linux hrtimers or macOS dispatch_after, but for kernel use.
 *
 * Features:
 *   - O(1) insert/delete of timers
 *   - Single-shot and repeating timers
 *   - Processed by PIT interrupt handler
 *   - No dynamic allocation (static timer structs)
 *
 * Usage:
 *   struct ktimer my_timer;
 *   ktimer_init(&my_timer, my_callback, my_arg);
 *   ktimer_start(&my_timer, 100);  // Fire in 100ms
 */

#ifndef KTIMER_H
#define KTIMER_H

#include <stdint.h>
#include <stdbool.h>
#include "list.h"
#include "spinlock.h"

/* Timer callback: receives user-provided argument */
typedef void (*ktimer_fn)(void *arg);

/* ── Timer structure ─────────────────────────────────────────── */

struct ktimer {
    struct list_node node;       /* Linkage in timer wheel slot */
    uint64_t         expires;    /* Absolute tick when timer fires */
    uint64_t         interval;   /* Repeat interval (0 = one-shot) */
    ktimer_fn        callback;   /* Function to call on expiry */
    void            *arg;        /* User argument to callback */
    bool             active;     /* Is this timer pending? */
};

/* ── Tick rate ──────────────────────────────────────────────── */

/* At 100 Hz PIT, each tick is 10 ms */
#define TIMER_TICK_MS     10

/* ── Timer wheel (256 slots, ~2.5 seconds at 100 Hz) ────────── */

#define TIMER_WHEEL_SIZE  256
#define TIMER_WHEEL_MASK  (TIMER_WHEEL_SIZE - 1)

/* ── API ─────────────────────────────────────────────────────── */

/* Initialize a timer (does not start it) */
static inline void ktimer_init(struct ktimer *t, ktimer_fn fn, void *arg) {
    t->callback = fn;
    t->arg = arg;
    t->expires = 0;
    t->interval = 0;
    t->active = false;
}

/* Start a one-shot timer (fires after delay_ms milliseconds) */
void ktimer_start(struct ktimer *t, uint64_t delay_ms);

/* Start a repeating timer (fires every interval_ms milliseconds) */
void ktimer_start_repeating(struct ktimer *t, uint64_t interval_ms);

/* Cancel a pending timer */
void ktimer_cancel(struct ktimer *t);

/* Process expired timers — called from PIT IRQ handler */
void ktimer_tick(void);

/* Initialize the timer wheel subsystem */
void ktimer_init_subsystem(void);

/* Get number of active timers (for diagnostics) */
uint32_t ktimer_active_count(void);

#endif /* KTIMER_H */
