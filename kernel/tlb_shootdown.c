/*
 * Anykernel OS — TLB Shootdown Implementation
 *
 * Cross-CPU TLB invalidation mechanism via IPI.
 *
 * Uses the LAPIC driver to send IPIs and signal EOI.
 * On single-core (active_cpus == 1) the broadcast is a safe no-op.
 * When SMP AP startup increments active_cpus, IPIs are sent
 * via lapic_send_ipi_all() from the LAPIC driver.
 */

#include "tlb_shootdown.h"
#include "lapic.h"
#include "percpu.h"
#include "kprintf.h"
#include "log.h"

/* ── Global shootdown state ─────────────────────────────────── */

struct tlb_shootdown_state tlb_sd = {
    .target_addr  = 0,
    .pending_cpus = 0,
    .ack_count    = 0,
    .lock         = SPINLOCK_INIT,
};

/* Busy-wait iteration limit before declaring a shootdown timeout */
#define TLB_SHOOTDOWN_TIMEOUT  1000000

/* Number of active CPUs. BSP = 1. SMP startup increments this. */
static volatile uint32_t active_cpus = 1;

/* ── Initialization ──────────────────────────────────────────────── */

void tlb_shootdown_init(void) {
    /* ISR stub registered in IDT by idt_init() (vector 0xFE -> isr_stub_254).
     * The stub in interrupts.asm saves registers, calls
     * tlb_shootdown_ipi_handler(), and executes iretq. */
    LOG_OK("TLB shootdown: initialized (vector 0x%x, ISR wired, %u CPUs active)",
           IPI_TLB_SHOOTDOWN, active_cpus);
}

/* ── Broadcast: ask all CPUs to invalidate ─────────────────────── */

void tlb_shootdown_broadcast(uint64_t virt) {
    /* Single-core: nothing to do, caller already did invlpg locally */
    if (active_cpus <= 1) return;

    uint64_t irq_flags;
    spin_lock_irqsave(&tlb_sd.lock, &irq_flags);

    /* Set up shared state */
    tlb_sd.target_addr  = virt;
    tlb_sd.pending_cpus = active_cpus - 1;  /* Exclude current CPU */
    tlb_sd.ack_count    = 0;

    /* Memory barrier: ensure state is visible before sending IPI */
    asm volatile("mfence" ::: "memory");

    /* Send IPI to all other CPUs via LAPIC driver */
    if (lapic_is_enabled()) {
        lapic_send_ipi_all(IPI_TLB_SHOOTDOWN);
    }

    /* Wait for all CPUs to acknowledge.
     * Busy-wait with pause to avoid saturating the bus.
     * Explicit timeout to prevent hang if a CPU doesn't respond. */
    uint64_t timeout = 0;
    while (__atomic_load_n(&tlb_sd.ack_count, __ATOMIC_ACQUIRE)
           < tlb_sd.pending_cpus) {
        asm volatile("pause");
        if (++timeout > TLB_SHOOTDOWN_TIMEOUT) {
            kprintf("[TLB] shootdown timeout: %u/%u CPUs acked\n",
                    tlb_sd.ack_count, tlb_sd.pending_cpus);
            break;
        }
    }

    spin_unlock_irqrestore(&tlb_sd.lock, irq_flags);
}

/* ── IPI Handler: executed on the receiving CPU ──────────────────── */

void tlb_shootdown_ipi_handler(void) {
    /* Read target address and execute invlpg */
    uint64_t addr = tlb_sd.target_addr;
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");

    /* Acknowledge that this CPU has invalidated */
    __atomic_fetch_add(&tlb_sd.ack_count, 1, __ATOMIC_RELEASE);

    /* EOI to LAPIC via driver.
     * On single-core without active LAPIC, this handler is never invoked
     * because there are no IPIs without LAPIC, so the check is extra protection. */
    lapic_eoi();
}

/* ── Query ───────────────────────────────────────────────────────── */

uint32_t tlb_shootdown_cpu_count(void) {
    return active_cpus;
}
