/*
 * Anykernel OS — TLB Shootdown via IPI
 *
 * Cuando una CPU modifica page tables (vmm_map/vmm_unmap), las demas
 * CPUs pueden tener entradas TLB obsoletas. Un IPI (Inter-Processor
 * Interrupt) fuerza a todas las CPUs a invalidar la entrada relevante.
 *
 * Protocolo:
 *   1. CPU emisora escribe la direccion virtual en tlb_shootdown_addr
 *   2. Envia IPI vector IPI_TLB_SHOOTDOWN a todas las demas CPUs
 *   3. Cada CPU receptora ejecuta invlpg sobre esa direccion
 *   4. Incrementa tlb_shootdown_ack atomicamente
 *   5. CPU emisora espera hasta que todos hayan respondido
 *
 * Requiere LAPIC para enviar IPIs reales. Mientras solo haya PIC 8259A
 * (single-core), el shootdown es un no-op seguro.
 */

#ifndef TLB_SHOOTDOWN_H
#define TLB_SHOOTDOWN_H

#include <stdint.h>
#include "spinlock.h"

/* Vector dedicado para el IPI de TLB shootdown.
 * Fuera del rango PIC (0x20-0x2F) y excepciones (0x00-0x1F).
 * Vector 0xFE es convencion comun (Linux usa 0xFD para reschedule). */
#define IPI_TLB_SHOOTDOWN  0xFE

/* Estado compartido del shootdown en curso */
struct tlb_shootdown_state {
    volatile uint64_t target_addr;    /* Direccion virtual a invalidar */
    volatile uint32_t pending_cpus;   /* Cuantas CPUs deben responder */
    volatile uint32_t ack_count;      /* Cuantas CPUs han respondido */
    spinlock_t        lock;           /* Protege el estado durante shootdown */
};

/* Estado global — definido en tlb_shootdown.c */
extern struct tlb_shootdown_state tlb_sd;

/*
 * Initialize TLB shootdown subsystem.
 * Registers the IPI handler in the IDT.
 */
void tlb_shootdown_init(void);

/*
 * Request all other CPUs to flush TLB for `virt`.
 * Blocks until all CPUs acknowledge (or no-ops if single-core).
 *
 * MUST be called AFTER the local invlpg (caller handles local flush).
 */
void tlb_shootdown_broadcast(uint64_t virt);

/*
 * IPI handler: called on receiving CPUs.
 * Reads target_addr, executes invlpg, increments ack_count.
 */
void tlb_shootdown_ipi_handler(void);

/*
 * Returns number of active CPUs (for shootdown targeting).
 * Currently returns 1 (BSP only). SMP startup will update this.
 */
uint32_t tlb_shootdown_cpu_count(void);

#endif /* TLB_SHOOTDOWN_H */
