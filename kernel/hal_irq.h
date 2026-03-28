/*
 * Anykernel OS — Hardware Abstraction Layer: IRQ Controller
 *
 * Polymorphic IRQ controller interface. Today we use the 8259A PIC,
 * but when we implement APIC/IOAPIC for SMP, we just register a
 * different driver — no kernel code changes needed.
 *
 * Inspired by: Linux irq_chip, macOS IOInterruptController.
 */

#ifndef HAL_IRQ_H
#define HAL_IRQ_H

#include <stdint.h>

/* ── IRQ controller operations ───────────────────────────────── */

struct hal_irq_ops {
    const char *name;                      /* e.g. "PIC-8259", "IOAPIC" */

    void (*init)(void);                     /* Initialize hardware */
    void (*eoi)(uint8_t irq);              /* End-of-interrupt */
    void (*mask)(uint8_t irq);             /* Disable an IRQ line */
    void (*unmask)(uint8_t irq);           /* Enable an IRQ line */
};

/* ── Global IRQ driver (set during boot) ─────────────────────── */

extern struct hal_irq_ops *hal_irq;

/* ── Polymorphic API ─────────────────────────────────────────── */

static inline void hal_irq_init(void) {
    if (hal_irq && hal_irq->init)
        hal_irq->init();
}

static inline void hal_irq_eoi(uint8_t irq) {
    if (hal_irq && hal_irq->eoi)
        hal_irq->eoi(irq);
}

static inline void hal_irq_mask(uint8_t irq) {
    if (hal_irq && hal_irq->mask)
        hal_irq->mask(irq);
}

static inline void hal_irq_unmask(uint8_t irq) {
    if (hal_irq && hal_irq->unmask)
        hal_irq->unmask(irq);
}

/* Register an IRQ controller driver (replaces current) */
void hal_irq_register(struct hal_irq_ops *ops);

#endif /* HAL_IRQ_H */
