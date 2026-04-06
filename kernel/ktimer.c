/*
 * Anykernel OS — Kernel Timer Wheel Implementation
 *
 * Uses a hashed wheel algorithm: timers are placed into slots
 * based on their expiry tick modulo the wheel size. On each PIT
 * tick, we process only the current slot — O(1) per tick.
 *
 * Timer wheel (256 slots at 100 Hz):
 *   Slot = expires % 256
 *   Each slot is a linked list of timers expiring at that slot
 *   One revolution of the wheel = 256 ticks = 2.56 seconds
 */

#include "ktimer.h"
#include "pic.h"
#include "log.h"
#include "compiler.h"

/* ── Timer wheel state ───────────────────────────────────────── */

static struct list_head timer_wheel[TIMER_WHEEL_SIZE];
static spinlock_t timer_lock = SPINLOCK_INIT;
static uint32_t active_timers = 0;
static bool timer_subsystem_ready = false;

/* ── Initialize ──────────────────────────────────────────────── */

void ktimer_init_subsystem(void) {
    for (int i = 0; i < TIMER_WHEEL_SIZE; i++)
        list_head_init(&timer_wheel[i]);
    active_timers = 0;
    timer_subsystem_ready = true;
    LOG_OK("Timer wheel initialized (256 slots, 100 Hz)");
}

/* ── Insert timer into wheel ─────────────────────────────────── */

static void timer_insert(struct ktimer *t) {
    uint32_t slot = (uint32_t)(t->expires & TIMER_WHEEL_MASK);
    list_push_back(&timer_wheel[slot], &t->node);
    t->active = true;
    active_timers++;
}

/* ── Start a one-shot timer ──────────────────────────────────── */

void ktimer_start(struct ktimer *t, uint64_t delay_ms) {
    uint64_t irq_flags;
    spin_lock_irqsave(&timer_lock, &irq_flags);

    /* Cancel if already active */
    if (t->active) {
        list_remove_node(&t->node);
        active_timers--;
    }

    uint64_t ticks = delay_ms / TIMER_TICK_MS;  /* 100 Hz = 10ms/tick */
    if (ticks == 0) ticks = 1;

    t->expires = pit_get_ticks() + ticks;
    t->interval = 0;  /* One-shot */
    timer_insert(t);

    spin_unlock_irqrestore(&timer_lock, irq_flags);
}

/* ── Start a repeating timer ─────────────────────────────────── */

void ktimer_start_repeating(struct ktimer *t, uint64_t interval_ms) {
    uint64_t irq_flags;
    spin_lock_irqsave(&timer_lock, &irq_flags);

    if (t->active) {
        list_remove_node(&t->node);
        active_timers--;
    }

    uint64_t ticks = interval_ms / TIMER_TICK_MS;
    if (ticks == 0) ticks = 1;

    t->expires = pit_get_ticks() + ticks;
    t->interval = ticks;
    timer_insert(t);

    spin_unlock_irqrestore(&timer_lock, irq_flags);
}

/* ── Cancel a timer ──────────────────────────────────────────── */

void ktimer_cancel(struct ktimer *t) {
    uint64_t irq_flags;
    spin_lock_irqsave(&timer_lock, &irq_flags);

    if (t->active) {
        list_remove_node(&t->node);
        t->active = false;
        active_timers--;
    }

    spin_unlock_irqrestore(&timer_lock, irq_flags);
}

/* ── Tick handler — called from PIT IRQ ──────────────────────── */

__hot void ktimer_tick(void) {
    if (unlikely(!timer_subsystem_ready)) return;

    uint64_t now = pit_get_ticks();
    uint32_t slot = (uint32_t)(now & TIMER_WHEEL_MASK);

    /* No lock needed if slot is empty (fast path) */
    if (list_empty(&timer_wheel[slot])) return;

    uint64_t irq_flags;
    spin_lock_irqsave(&timer_lock, &irq_flags);

    struct list_node *pos, *tmp;
    list_for_each_safe(pos, tmp, &timer_wheel[slot]) {
        struct ktimer *t = container_of(pos, struct ktimer, node);

        if (now >= t->expires) {
            list_remove_node(pos);
            t->active = false;
            active_timers--;

            /* Call the callback (with lock released) */
            spin_unlock_irqrestore(&timer_lock, irq_flags);
            t->callback(t->arg);
            spin_lock_irqsave(&timer_lock, &irq_flags);

            /* Re-arm if repeating */
            if (t->interval > 0) {
                t->expires = now + t->interval;
                timer_insert(t);
            }
        }
    }

    spin_unlock_irqrestore(&timer_lock, irq_flags);
}

/* ── Diagnostics ─────────────────────────────────────────────── */

uint32_t ktimer_active_count(void) {
    return active_timers;
}
