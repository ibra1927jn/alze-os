/*
 * Anykernel OS — Local APIC (LAPIC) Driver
 *
 * El LAPIC (Local Advanced Programmable Interrupt Controller) es el
 * controlador de interrupciones por CPU en x86_64. Cada core tiene
 * su propio LAPIC mapeado en memoria (MMIO) en 0xFEE00000.
 *
 * Funciones principales:
 *   - Enviar/recibir IPIs (Inter-Processor Interrupts) entre CPUs
 *   - Timer local por CPU (one-shot o periodico)
 *   - End-of-Interrupt (EOI) para liberar la linea de interrupcion
 *   - Identificacion de la CPU actual via LAPIC ID
 *
 * Referencia: Intel SDM Vol. 3A, Chapter 10 — APIC
 */

#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

/* ── LAPIC base fisica (estandar x86_64) ────────────────────────── */

#define LAPIC_BASE_PHYS     0xFEE00000UL

/* ── Offsets de registros LAPIC (Table 10-1, Intel SDM) ─────────── */

#define LAPIC_REG_ID            0x020   /* LAPIC ID (RO) */
#define LAPIC_REG_VERSION       0x030   /* Version (RO) */
#define LAPIC_REG_TPR           0x080   /* Task Priority Register */
#define LAPIC_REG_APR           0x090   /* Arbitration Priority (RO) */
#define LAPIC_REG_PPR           0x0A0   /* Processor Priority (RO) */
#define LAPIC_REG_EOI           0x0B0   /* End-Of-Interrupt (WO) */
#define LAPIC_REG_RRD           0x0C0   /* Remote Read (RO) */
#define LAPIC_REG_LDR           0x0D0   /* Logical Destination */
#define LAPIC_REG_DFR           0x0E0   /* Destination Format */
#define LAPIC_REG_SVR           0x0F0   /* Spurious Interrupt Vector */
#define LAPIC_REG_ISR_BASE      0x100   /* In-Service Register (8x 32-bit) */
#define LAPIC_REG_TMR_BASE      0x180   /* Trigger Mode Register (8x 32-bit) */
#define LAPIC_REG_IRR_BASE      0x200   /* Interrupt Request Register (8x 32-bit) */
#define LAPIC_REG_ESR           0x280   /* Error Status Register */
#define LAPIC_REG_ICR_LOW       0x300   /* Interrupt Command Register (low 32) */
#define LAPIC_REG_ICR_HIGH      0x310   /* Interrupt Command Register (high 32) */
#define LAPIC_REG_TIMER_LVT     0x320   /* LVT Timer */
#define LAPIC_REG_THERMAL_LVT   0x330   /* LVT Thermal Sensor */
#define LAPIC_REG_PERFCNT_LVT   0x340   /* LVT Performance Counter */
#define LAPIC_REG_LINT0_LVT     0x350   /* LVT LINT0 */
#define LAPIC_REG_LINT1_LVT     0x360   /* LVT LINT1 */
#define LAPIC_REG_ERROR_LVT     0x370   /* LVT Error */
#define LAPIC_REG_TIMER_ICR     0x380   /* Timer Initial Count */
#define LAPIC_REG_TIMER_CCR     0x390   /* Timer Current Count (RO) */
#define LAPIC_REG_TIMER_DCR     0x3E0   /* Timer Divide Configuration */

/* ── Bits del SVR (Spurious Interrupt Vector Register) ──────────── */

#define LAPIC_SVR_ENABLE        (1 << 8)    /* Bit 8: habilita el LAPIC */
#define LAPIC_SVR_VECTOR        0xFF        /* Vector 0xFF para spurious */

/* ── ICR: delivery mode, destination shorthand ──────────────────── */

#define LAPIC_ICR_FIXED         (0 << 8)    /* Delivery: Fixed */
#define LAPIC_ICR_SMI           (2 << 8)    /* Delivery: SMI */
#define LAPIC_ICR_NMI           (4 << 8)    /* Delivery: NMI */
#define LAPIC_ICR_INIT          (5 << 8)    /* Delivery: INIT */
#define LAPIC_ICR_STARTUP       (6 << 8)    /* Delivery: SIPI */

#define LAPIC_ICR_PHYSICAL      (0 << 11)   /* Destination mode: fisico */
#define LAPIC_ICR_LOGICAL       (1 << 11)   /* Destination mode: logico */

#define LAPIC_ICR_IDLE          (0 << 12)   /* Delivery status: idle */
#define LAPIC_ICR_PENDING       (1 << 12)   /* Delivery status: pending */

#define LAPIC_ICR_ASSERT        (1 << 14)   /* Level: assert */
#define LAPIC_ICR_DEASSERT      (0 << 14)   /* Level: de-assert */

#define LAPIC_ICR_EDGE          (0 << 15)   /* Trigger: edge */
#define LAPIC_ICR_LEVEL         (1 << 15)   /* Trigger: level */

#define LAPIC_ICR_DEST_NONE     (0 << 18)   /* Shorthand: ninguno (usar campo dest) */
#define LAPIC_ICR_DEST_SELF     (1 << 18)   /* Shorthand: solo esta CPU */
#define LAPIC_ICR_DEST_ALL      (2 << 18)   /* Shorthand: todas las CPUs */
#define LAPIC_ICR_DEST_ALL_EX   (3 << 18)   /* Shorthand: todas excepto esta */

/* ── LVT Timer: modo y mascara ──────────────────────────────────── */

#define LAPIC_TIMER_ONESHOT     (0 << 17)   /* One-shot */
#define LAPIC_TIMER_PERIODIC    (1 << 17)   /* Periodico */
#define LAPIC_TIMER_TSC_DEADLINE (2 << 17)  /* TSC-Deadline (si soportado) */
#define LAPIC_TIMER_MASKED      (1 << 16)   /* Mascara la interrupcion */

/* ── Timer divide values (DCR) ──────────────────────────────────── */

#define LAPIC_TIMER_DIV_1       0x0B
#define LAPIC_TIMER_DIV_2       0x00
#define LAPIC_TIMER_DIV_4       0x01
#define LAPIC_TIMER_DIV_8       0x02
#define LAPIC_TIMER_DIV_16      0x03
#define LAPIC_TIMER_DIV_32      0x08
#define LAPIC_TIMER_DIV_64      0x09
#define LAPIC_TIMER_DIV_128     0x0A

/* ── Vector del timer LAPIC ─────────────────────────────────────── */

#define LAPIC_TIMER_VECTOR      0xFD    /* Vector para la interrupcion del timer */
#define LAPIC_SPURIOUS_VECTOR   0xFF    /* Vector para interrupciones espurias */

/* ── API publica ────────────────────────────────────────────────── */

/*
 * Read a 32-bit LAPIC register at the given offset.
 * Accede via MMIO usando HHDM (PHYS2VIRT).
 */
uint32_t lapic_read(uint32_t reg);

/*
 * Write a 32-bit value to a LAPIC register.
 * Accede via MMIO usando HHDM (PHYS2VIRT).
 */
void lapic_write(uint32_t reg, uint32_t val);

/*
 * Initialize the local APIC on this CPU.
 * Habilita el LAPIC via SVR, configura spurious vector,
 * limpia el TPR para aceptar todas las interrupciones.
 */
void lapic_init(void);

/*
 * Return the LAPIC ID of the current CPU.
 * Util para identificar que CPU esta ejecutando.
 */
uint32_t lapic_get_id(void);

/*
 * Send End-Of-Interrupt to the LAPIC.
 * DEBE llamarse al final de cada handler de interrupcion LAPIC.
 */
void lapic_eoi(void);

/*
 * Send an IPI (Inter-Processor Interrupt) to a specific CPU.
 * target_apic_id: LAPIC ID de la CPU destino.
 * vector: numero de interrupcion a enviar.
 */
void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector);

/*
 * Broadcast an IPI to ALL other CPUs (excluding self).
 * Usa destination shorthand "All Excluding Self".
 */
void lapic_send_ipi_all(uint8_t vector);

/*
 * Initialize the LAPIC timer in periodic mode.
 * frequency: frecuencia deseada en Hz (ej: 100 para 10ms tick).
 * Calibra el timer contra el PIT como fuente de referencia.
 */
void lapic_timer_init(uint32_t frequency);

/*
 * Stop the LAPIC timer (mask the LVT entry).
 */
void lapic_timer_stop(void);

/*
 * Returns 1 if LAPIC has been initialized, 0 otherwise.
 * Usado por TLB shootdown para saber si puede enviar IPIs.
 */
int lapic_is_enabled(void);

/*
 * LAPIC timer interrupt handler (called from ISR stub 253).
 * Incrementa tick counter y envia EOI.
 */
void lapic_timer_handler_c(void);

/*
 * Returns the LAPIC timer tick count since initialization.
 */
uint64_t lapic_timer_get_ticks(void);

#endif /* LAPIC_H */
