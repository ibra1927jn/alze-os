/*
 * Anykernel OS — TLB Shootdown Implementation
 *
 * Mecanismo de invalidacion TLB cross-CPU via IPI.
 *
 * Estado actual: single-core (PIC 8259A), el broadcast es un no-op.
 * Cuando se implemente LAPIC/SMP, solo hay que rellenar lapic_send_ipi()
 * y actualizar active_cpus. La infraestructura ya esta lista.
 */

#include "tlb_shootdown.h"
#include "memory.h"
#include "percpu.h"
#include "kprintf.h"
#include "log.h"

/* ── Estado global del shootdown ─────────────────────────────── */

struct tlb_shootdown_state tlb_sd = {
    .target_addr  = 0,
    .pending_cpus = 0,
    .ack_count    = 0,
    .lock         = SPINLOCK_INIT,
};

/* Numero de CPUs activas. BSP = 1. SMP startup incrementa esto. */
static volatile uint32_t active_cpus = 1;

/* ── Stub: enviar IPI via LAPIC ──────────────────────────────── */

/*
 * Envia un IPI a todas las CPUs excepto la actual.
 * TODO (SMP): escribir al LAPIC ICR (Interrupt Command Register):
 *   - ICR Low:  vector | delivery=fixed | dest=all-excluding-self
 *   - ICR High: 0 (broadcast)
 *
 * LAPIC ICR (registro 0x300/0x310 del LAPIC MMIO):
 *   Bits 0-7:   Vector (IPI_TLB_SHOOTDOWN = 0xFE)
 *   Bits 8-10:  Delivery Mode (000 = Fixed)
 *   Bit 11:     Destination Mode (0 = Physical)
 *   Bit 12:     Delivery Status (read-only)
 *   Bit 14:     Level (1 = Assert)
 *   Bit 15:     Trigger (0 = Edge)
 *   Bits 18-19: Destination Shorthand (11 = All Excluding Self)
 */
static inline void lapic_send_ipi_all_others(uint8_t vector) {
    /* Sin LAPIC todavia — no-op seguro en single-core.
     * Cuando LAPIC este disponible:
     *   volatile uint32_t *icr_low  = (uint32_t *)PHYS2VIRT(0xFEE00300);
     *   volatile uint32_t *icr_high = (uint32_t *)PHYS2VIRT(0xFEE00310);
     *   *icr_high = 0;
     *   *icr_low  = vector | (0b11 << 18);  // All Excluding Self
     */
    (void)vector;
}

/* ── Inicializacion ──────────────────────────────────────────── */

void tlb_shootdown_init(void) {
    /* ISR stub registrado en IDT por idt_init() (vector 0xFE → isr_stub_254).
     * El stub en interrupts.asm guarda registros, llama a
     * tlb_shootdown_ipi_handler(), y ejecuta iretq. */
    LOG_OK("TLB shootdown: initialized (vector 0x%x, ISR wired, %u CPUs active)",
           IPI_TLB_SHOOTDOWN, active_cpus);
}

/* ── Broadcast: pedir a todas las CPUs que invaliden ─────────── */

void tlb_shootdown_broadcast(uint64_t virt) {
    /* Single-core: nada que hacer, el caller ya hizo invlpg local */
    if (active_cpus <= 1) return;

    uint64_t irq_flags;
    spin_lock_irqsave(&tlb_sd.lock, &irq_flags);

    /* Configurar el estado compartido */
    tlb_sd.target_addr  = virt;
    tlb_sd.pending_cpus = active_cpus - 1;  /* Excluir la CPU actual */
    tlb_sd.ack_count    = 0;

    /* Barrera de memoria: asegurar que el estado es visible antes del IPI */
    asm volatile("mfence" ::: "memory");

    /* Enviar IPI a todas las demas CPUs */
    lapic_send_ipi_all_others(IPI_TLB_SHOOTDOWN);

    /* Esperar a que todas las CPUs confirmen.
     * Busy-wait con pause para no saturar el bus.
     * Timeout implicito: si una CPU no responde, algo esta muy mal. */
    while (__atomic_load_n(&tlb_sd.ack_count, __ATOMIC_ACQUIRE)
           < tlb_sd.pending_cpus) {
        asm volatile("pause");
    }

    spin_unlock_irqrestore(&tlb_sd.lock, irq_flags);
}

/* ── IPI Handler: ejecutado en la CPU receptora ──────────────── */

void tlb_shootdown_ipi_handler(void) {
    /* Leer la direccion objetivo y ejecutar invlpg */
    uint64_t addr = tlb_sd.target_addr;
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");

    /* Confirmar que esta CPU ya invalido */
    __atomic_fetch_add(&tlb_sd.ack_count, 1, __ATOMIC_RELEASE);

    /* EOI al LAPIC.
     * El LAPIC EOI register esta en 0xFEE000B0 (fisico).
     * Escribir cualquier valor (convencion: 0) senala fin de interrupcion.
     * En single-core sin LAPIC activo, esta escritura es inofensiva
     * porque el handler nunca se invoca (no hay IPI sin LAPIC). */
    volatile uint32_t *lapic_eoi = (volatile uint32_t *)PHYS2VIRT(0xFEE000B0);
    *lapic_eoi = 0;
}

/* ── Query ───────────────────────────────────────────────────── */

uint32_t tlb_shootdown_cpu_count(void) {
    return active_cpus;
}
