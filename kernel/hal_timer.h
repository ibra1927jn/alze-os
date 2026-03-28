/*
 * Anykernel OS — Hardware Abstraction Layer: Timer
 *
 * Polymorphic timer interface that decouples the kernel from
 * the specific timer hardware. Today we use the PIT (8254),
 * tomorrow we can swap in HPET, APIC Timer, or TSC deadline
 * without changing any kernel code.
 *
 * Inspired by: Linux clocksource/clockevent, macOS mach_absolute_time.
 */

#ifndef HAL_TIMER_H
#define HAL_TIMER_H

#include <stdint.h>

/* ── Timer driver operations ─────────────────────────────────── */

struct hal_timer_ops {
    const char *name;               /* e.g. "PIT", "HPET", "APIC" */
    uint32_t    frequency_hz;       /* Ticks per second */

    void     (*init)(uint32_t freq_hz);   /* Initialize hardware */
    uint64_t (*get_ticks)(void);           /* Read tick counter */
    void     (*tick_handler)(void);        /* Called from IRQ */
};

/* ── Global timer driver (set during boot) ───────────────────── */

extern struct hal_timer_ops *hal_timer;

/* ── Polymorphic API (indirects through hal_timer) ───────────── */

static inline void hal_timer_init(uint32_t freq_hz) {
    if (hal_timer && hal_timer->init)
        hal_timer->init(freq_hz);
}

static inline uint64_t hal_timer_get_ticks(void) {
    if (hal_timer && hal_timer->get_ticks)
        return hal_timer->get_ticks();
    return 0;
}

static inline void hal_timer_tick(void) {
    if (hal_timer && hal_timer->tick_handler)
        hal_timer->tick_handler();
}

/* Register a timer driver (replaces current) */
void hal_timer_register(struct hal_timer_ops *ops);

#endif /* HAL_TIMER_H */
