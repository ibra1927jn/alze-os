/*
 * Anykernel OS — TLB Shootdown via IPI
 *
 * When a CPU modifies page tables (vmm_map/vmm_unmap), other CPUs
 * may have stale TLB entries. An IPI (Inter-Processor Interrupt)
 * forces all CPUs to invalidate the relevant entry.
 *
 * Protocol:
 *   1. Sending CPU writes the virtual address to tlb_shootdown_addr
 *   2. Sends IPI vector IPI_TLB_SHOOTDOWN to all other CPUs
 *   3. Each receiving CPU executes invlpg on that address
 *   4. Atomically increments tlb_shootdown_ack
 *   5. Sending CPU waits until all have responded
 *
 * Requires LAPIC for real IPIs. While only PIC 8259A is present
 * (single-core), the shootdown is a safe no-op.
 */

#ifndef TLB_SHOOTDOWN_H
#define TLB_SHOOTDOWN_H

#include <stdint.h>
#include "spinlock.h"

/* Dedicated vector for TLB shootdown IPI.
 * Outside PIC range (0x20-0x2F) and exceptions (0x00-0x1F).
 * Vector 0xFE is a common convention (Linux uses 0xFD for reschedule). */
#define IPI_TLB_SHOOTDOWN  0xFE

/* Shared state for an in-progress shootdown */
struct tlb_shootdown_state {
    volatile uint64_t target_addr;    /* Virtual address to invalidate */
    volatile uint32_t pending_cpus;   /* How many CPUs must respond */
    volatile uint32_t ack_count;      /* How many CPUs have responded */
    spinlock_t        lock;           /* Protects state during shootdown */
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
