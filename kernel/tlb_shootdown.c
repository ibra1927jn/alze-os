/*
 * Anykernel OS — TLB Shootdown Implementation
 *
 * Mecanismo de invalidacion TLB cross-CPU via IPI.
 *
 * Usa el driver LAPIC para enviar IPIs y senalar EOI.
 * En single-core (active_cpus == 1) el broadcast es un no-op seguro.
 * Cuando SMP AP startup incremente active_cpus, los IPIs se envian
 * via lapic_send_ipi_all() del driver LAPIC.
 */

#include "tlb_shootdown.h"
#include "lapic.h"
#include "memory.h"
#include "percpu.h"
#include "kprintf.h"
#include "log.h"

/* ── Estado global del shootdown ─────────────────────────────────── */

struct tlb_shootdown_state tlb_sd = {
    .target_addr  = 0,
    .pending_cpus = 0,
    .ack_count    = 0,
    .lock         = SPINLOCK_INIT,
};

/* Busy-wait iteration limit before declaring a shootdown timeout */
#define TLB_SHOOTDOWN_TIMEOUT  1000000

/* Numero de CPUs activas. BSP = 1. SMP startup incrementa esto. */
static volatile uint32_t active_cpus = 1;

/* ── Inicializacion ──────────────────────────────────────────────── */

void tlb_shootdown_init(void) {
    /* ISR stub registrado en IDT por idt_init() (vector 0xFE → isr_stub_254).
     * El stub en interrupts.asm guarda registros, llama a
     * tlb_shootdown_ipi_handler(), y ejecuta iretq. */
    LOG_OK("TLB shootdown: initialized (vector 0x%x, ISR wired, %u CPUs active)",
           IPI_TLB_SHOOTDOWN, active_cpus);
}

/* ── Broadcast: pedir a todas las CPUs que invaliden ─────────────── */

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

    /* Enviar IPI a todas las demas CPUs via LAPIC driver */
    if (lapic_is_enabled()) {
        lapic_send_ipi_all(IPI_TLB_SHOOTDOWN);
    }

    /* Esperar a que todas las CPUs confirmen.
     * Busy-wait con pause para no saturar el bus.
     * Timeout explicito para evitar hang si una CPU no responde. */
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

/* ── IPI Handler: ejecutado en la CPU receptora ──────────────────── */

void tlb_shootdown_ipi_handler(void) {
    /* Leer la direccion objetivo y ejecutar invlpg */
    uint64_t addr = tlb_sd.target_addr;
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");

    /* Confirmar que esta CPU ya invalido */
    __atomic_fetch_add(&tlb_sd.ack_count, 1, __ATOMIC_RELEASE);

    /* EOI al LAPIC via driver.
     * En single-core sin LAPIC activo, este handler nunca se invoca
     * porque no hay IPI sin LAPIC, asi que el check es una proteccion extra. */
    lapic_eoi();
}

/* ── Query ───────────────────────────────────────────────────────── */

uint32_t tlb_shootdown_cpu_count(void) {
    return active_cpus;
}
