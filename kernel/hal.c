/*
 * Anykernel OS — HAL Implementation
 *
 * Registers current hardware drivers (PIC + PIT) as HAL backends.
 * When APIC/HPET are implemented, they replace these registrations.
 */

#include "hal_timer.h"
#include "hal_irq.h"
#include "pic.h"
#include "kprintf.h"
#include "log.h"

/* ── Global HAL state ────────────────────────────────────────── */

struct hal_timer_ops *hal_timer = 0;
struct hal_irq_ops  *hal_irq  = 0;

/* ── PIT as HAL timer backend ────────────────────────────────── */

static struct hal_timer_ops pit_timer_ops = {
    .name         = "PIT-8254",
    .frequency_hz = 100,
    .init         = pit_init,
    .get_ticks    = pit_get_ticks,
    .tick_handler = pit_tick,
};

/* ── PIC as HAL IRQ backend ──────────────────────────────────── */

static struct hal_irq_ops pic_irq_ops = {
    .name   = "PIC-8259A",
    .init   = pic_init,
    .eoi    = pic_eoi,
    .mask   = pic_mask,
    .unmask = pic_unmask,
};

/* ── Register drivers ────────────────────────────────────────── */

void hal_timer_register(struct hal_timer_ops *ops) {
    hal_timer = ops;
    LOG_OK("HAL: timer driver '%s' (%u Hz)", ops->name, ops->frequency_hz);
}

void hal_irq_register(struct hal_irq_ops *ops) {
    hal_irq = ops;
    LOG_OK("HAL: IRQ controller '%s'", ops->name);
}

/* ── Initialize HAL with current hardware ────────────────────── */

void hal_init(void) {
    hal_irq_register(&pic_irq_ops);
    hal_timer_register(&pit_timer_ops);
    LOG_OK("HAL: hardware abstraction layer initialized");
}
