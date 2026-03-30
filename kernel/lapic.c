/*
 * Anykernel OS — Local APIC (LAPIC) Driver Implementation
 *
 * Full LAPIC controller implementation for x86_64:
 *   - MMIO read/write on the standard base 0xFEE00000
 *   - Initialization: enable SVR, clear TPR, configure LVT
 *   - EOI: release interrupt line
 *   - IPI: send inter-processor interrupts (point-to-point and broadcast)
 *   - Timer: calibrated against PIT, configurable periodic mode
 *
 * The LAPIC is accessed via HHDM (Higher Half Direct Map) since its
 * registers are physically mapped at 0xFEE00000.
 *
 * Reference: Intel SDM Vol. 3A, Chapter 10
 */

#include "lapic.h"
#include "memory.h"
#include "io.h"
#include "pic.h"
#include "kprintf.h"
#include "log.h"

/* ── MSR and port constants ─────────────────────────────────────── */

#define MSR_IA32_APIC_BASE     0x1B           /* IA32_APIC_BASE MSR address    */
#define APIC_GLOBAL_ENABLE     (1 << 11)      /* Bit 11: APIC Global Enable    */
#define APIC_BASE_ADDR_MASK    0xFFFFF000UL   /* Bits 12..35: base address     */

/* PIT channel 2 calibration ports */
#define PORT_SPEAKER_GATE      0x61           /* Speaker gate / PIT ch2 gate   */
#define PORT_PIT_CH2_DATA      0x42           /* PIT channel 2 data port       */
#define PORT_PIT_CMD           0x43           /* PIT command/mode register      */

/* PIT channel 2 gate/speaker bit masks */
#define SPEAKER_GATE_ON        0x01           /* Bit 0: enable ch2 gate        */
#define SPEAKER_OFF_MASK       0xFD           /* Clear bit 1: disable speaker  */
#define PIT_CH2_OUTPUT_BIT     0x20           /* Bit 5: ch2 output status      */

/* PIT command for channel 2 calibration */
#define PIT_CMD_CH2_ONESHOT    0xB0           /* Ch2, lo/hi byte, mode 0       */

/* LAPIC timer calibration constants */
#define LAPIC_TIMER_MAX_COUNT  0xFFFFFFFF     /* Max initial count (32-bit)    */
#define PIT_CALIBRATION_TIMEOUT 0xFFFFFF      /* Spin-wait timeout iterations  */
#define LAPIC_FALLBACK_TICKS   100000         /* Fallback elapsed for ~10ms    */

/* LAPIC version register bit fields */
#define LAPIC_VERSION_MASK     0xFF           /* Bits 0-7: version number      */
#define LAPIC_MAX_LVT_SHIFT    16             /* Bits 16-23: max LVT entry     */
#define LAPIC_MAX_LVT_MASK     0xFF           /* 8-bit field width             */

/* LAPIC ID register */
#define LAPIC_ID_SHIFT         24             /* Bits 24-31: LAPIC ID          */
#define LAPIC_ID_MASK          0xFF           /* 8-bit field width             */

/* Destination Format Register: flat model */
#define LAPIC_DFR_FLAT_MODEL   0xFFFFFFFF     /* All bits 1 = flat model       */

/* Logical Destination Register */
#define LAPIC_LDR_SHIFT        24             /* LDR bitmask in bits 24-31     */
#define LAPIC_MAX_FLAT_CPUS    8              /* Flat model supports 8 CPUs    */

/* ── Internal state ─────────────────────────────────────────────── */

/* Flag: 1 if the LAPIC was initialized successfully */
static volatile int lapic_enabled = 0;

/* LAPIC timer ticks per second (computed during calibration) */
static uint32_t lapic_ticks_per_sec = 0;


/* ── MMIO: LAPIC register access ─────────────────────────────── */

uint32_t lapic_read(uint32_t reg) {
    /* Each LAPIC register is a uint32_t at base + offset.
     * Accessed via HHDM to obtain the virtual address. */
    volatile uint32_t *addr = (volatile uint32_t *)PHYS2VIRT(LAPIC_BASE_PHYS + reg);
    return *addr;
}

void lapic_write(uint32_t reg, uint32_t val) {
    volatile uint32_t *addr = (volatile uint32_t *)PHYS2VIRT(LAPIC_BASE_PHYS + reg);
    *addr = val;
}

/* ── Wait for ICR to become idle ────────────────────────────── */

/*
 * The LAPIC may take time to send an IPI. Bit 12 (Delivery Status)
 * of ICR Low indicates a pending send. We wait until it is 0 (idle)
 * before writing a new IPI.
 */
static void lapic_wait_icr_idle(void) {
    while (lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_PENDING) {
        asm volatile("pause");
    }
}

/* ── Verify LAPIC support via MSR ────────────────────────────── */

/*
 * Reads MSR IA32_APIC_BASE (0x1B) to verify that the LAPIC is
 * hardware-enabled and to obtain the physical base address.
 * Bit 11: APIC Global Enable.
 * Bits 12-35: Base Address (should be 0xFEE00000).
 */
static int lapic_check_msr(void) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_IA32_APIC_BASE));

    /* Check bit 11: APIC globally enabled */
    if (!(lo & APIC_GLOBAL_ENABLE)) {
        LOG_WARN("LAPIC: APIC Global Enable bit not set in IA32_APIC_BASE MSR");
        /* Try to enable it */
        lo |= APIC_GLOBAL_ENABLE;
        asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(MSR_IA32_APIC_BASE));
        LOG_INFO("LAPIC: enabled APIC via IA32_APIC_BASE MSR");
    }

    /* Extract physical base (bits 12..35, 4KB-aligned) */
    uint64_t base = ((uint64_t)hi << 32) | (lo & APIC_BASE_ADDR_MASK);
    if (base != LAPIC_BASE_PHYS) {
        LOG_WARN("LAPIC: base address 0x%lx differs from standard 0x%lx",
                 base, (uint64_t)LAPIC_BASE_PHYS);
        /* We use the standard one anyway, HHDM already maps it */
    }

    return 1;
}

/* ── LAPIC timer calibration against PIT ──────────────────── */

/*
 * Strategy: use the PIT as a known time reference.
 * 1. Configure PIT channel 2 in one-shot for ~10ms (11932 ticks at 1.193182 MHz)
 * 2. Start LAPIC timer with initial count = 0xFFFFFFFF
 * 3. Wait for PIT channel 2 to expire
 * 4. Read how much the LAPIC timer decremented
 * 5. Scale to ticks per second
 *
 * Precision: ±1% is sufficient for scheduling; HPET or TSC
 * can improve this if better precision is needed.
 */
static uint32_t lapic_calibrate_timer(void) {
    /* Configure LAPIC timer: divisor 16, one-shot, masked */
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_TIMER_MASKED);

    /*
     * Use PIT channel 2 as reference.
     * PIT channel 2 is connected to the speaker gate (port 0x61).
     * Configure in one-shot mode 0 with a period of ~10ms.
     *
     * PIT freq = 1193182 Hz -> ~10ms = 11932 ticks
     */
    #define CALIBRATION_MS          10
    #define CALIBRATION_PIT_TICKS   ((uint16_t)(PIT_BASE_FREQ * CALIBRATION_MS / 1000))

    /* Disable speaker, enable gate for channel 2 */
    uint8_t gate = inb(PORT_SPEAKER_GATE);
    gate = (gate & SPEAKER_OFF_MASK) | SPEAKER_GATE_ON;
    outb(PORT_SPEAKER_GATE, gate);

    /* PIT channel 2, mode 0 (interrupt on terminal count), lo/hi byte */
    outb(PORT_PIT_CMD, PIT_CMD_CH2_ONESHOT);
    outb(PORT_PIT_CH2_DATA, CALIBRATION_PIT_TICKS & 0xFF);
    outb(PORT_PIT_CH2_DATA, (CALIBRATION_PIT_TICKS >> 8) & 0xFF);

    /* Reset the gate to start the PIT channel 2 countdown */
    gate = inb(PORT_SPEAKER_GATE);
    outb(PORT_SPEAKER_GATE, gate & ~SPEAKER_GATE_ON);  /* Gate off */
    outb(PORT_SPEAKER_GATE, gate | SPEAKER_GATE_ON);   /* Gate on — start */

    /* Start LAPIC timer with maximum count */
    lapic_write(LAPIC_REG_TIMER_ICR, LAPIC_TIMER_MAX_COUNT);

    /* Wait for PIT channel 2 to reach 0.
     * Bit 5 of port 0x61 indicates channel 2 output.
     * When the countdown ends, output goes to 1 (mode 0).
     * Timeout to prevent hang if PIT does not respond. */
    uint32_t pit_timeout = PIT_CALIBRATION_TIMEOUT;
    while (!(inb(PORT_SPEAKER_GATE) & PIT_CH2_OUTPUT_BIT) && --pit_timeout > 0) {
        asm volatile("pause");
    }

    /* Read how much the LAPIC timer decremented */
    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    uint32_t elapsed = LAPIC_TIMER_MAX_COUNT - remaining;

    /* If PIT did not respond, use a reasonable fallback */
    if (pit_timeout == 0) {
        kprintf("[LAPIC] PIT calibration timeout, using fallback\n");
        elapsed = LAPIC_FALLBACK_TICKS;
    }

    /* Stop the LAPIC timer */
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_TIMER_MASKED);

    /* Scale to ticks per second.
     * elapsed ticks occurred in ~10ms -> ticks/sec = elapsed * 100.
     * Use 64-bit multiply to avoid overflow if elapsed > ~42M ticks. */
    uint64_t ticks64 = (uint64_t)elapsed * 100;
    if (ticks64 > UINT32_MAX) ticks64 = UINT32_MAX;
    uint32_t ticks_per_sec = (uint32_t)ticks64;

    LOG_INFO("LAPIC timer: calibrated %u ticks in ~10ms → %u ticks/sec",
             elapsed, ticks_per_sec);

    return ticks_per_sec;
}

/* ── LAPIC initialization ───────────────────────────────────── */

void lapic_init(void) {
    /* Verify and enable LAPIC via MSR */
    if (!lapic_check_msr()) {
        LOG_ERROR("LAPIC: failed to enable via MSR, aborting init");
        return;
    }

    /* Read version for diagnostics */
    uint32_t version = lapic_read(LAPIC_REG_VERSION);
    uint32_t max_lvt = ((version >> LAPIC_MAX_LVT_SHIFT) & LAPIC_MAX_LVT_MASK) + 1;
    LOG_INFO("LAPIC: version 0x%x, max LVT entries: %u",
             version & LAPIC_VERSION_MASK, max_lvt);

    /* Clear TPR: accept all interrupts (priority 0) */
    lapic_write(LAPIC_REG_TPR, 0);

    /* Configure Destination Format Register: flat model (all bits set to 1) */
    lapic_write(LAPIC_REG_DFR, LAPIC_DFR_FLAT_MODEL);

    /* Configure Logical Destination Register: bit for this CPU.
     * In flat model, LDR bits 24-31 are an 8-bit bitmask.
     * If LAPIC ID >= 8, use bit 0 as a safe fallback. */
    uint32_t id = lapic_get_id();
    uint32_t ldr_bit = (id < LAPIC_MAX_FLAT_CPUS) ? (1U << id) : 1U;
    lapic_write(LAPIC_REG_LDR, ldr_bit << LAPIC_LDR_SHIFT);

    /* Enable LAPIC via SVR:
     * Bit 8 = Enable, bits 0-7 = spurious vector (0xFF) */
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    /* Clear Error Status Register (write twice per Intel SDM) */
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);

    /* Mask LVT entries we do not use yet */
    lapic_write(LAPIC_REG_THERMAL_LVT, LAPIC_TIMER_MASKED);
    lapic_write(LAPIC_REG_PERFCNT_LVT, LAPIC_TIMER_MASKED);
    lapic_write(LAPIC_REG_LINT0_LVT,   LAPIC_TIMER_MASKED);
    lapic_write(LAPIC_REG_LINT1_LVT,   LAPIC_TIMER_MASKED);
    lapic_write(LAPIC_REG_ERROR_LVT,   LAPIC_TIMER_MASKED);
    lapic_write(LAPIC_REG_TIMER_LVT,   LAPIC_TIMER_MASKED);

    /* Send EOI in case any pending interrupt remains */
    lapic_eoi();

    /* Calibrate timer (required before lapic_timer_init) */
    lapic_ticks_per_sec = lapic_calibrate_timer();

    /* Mark as enabled */
    lapic_enabled = 1;

    uint32_t lapic_id = lapic_get_id();
    LOG_OK("LAPIC: initialized (ID=%u, base=0x%lx, spurious=0x%x)",
           lapic_id, (uint64_t)LAPIC_BASE_PHYS, LAPIC_SPURIOUS_VECTOR);
}

/* ── Queries ────────────────────────────────────────────────────── */

uint32_t lapic_get_id(void) {
    /* LAPIC ID is in bits 24-31 of the ID register */
    return (lapic_read(LAPIC_REG_ID) >> LAPIC_ID_SHIFT) & LAPIC_ID_MASK;
}

int lapic_is_enabled(void) {
    return lapic_enabled;
}

/* ── End-Of-Interrupt ───────────────────────────────────────────── */

void lapic_eoi(void) {
    /* Writing 0 to the EOI register signals end-of-interrupt.
     * Any value works but 0 is the convention. */
    lapic_write(LAPIC_REG_EOI, 0);
}

/* ── IPI: Inter-Processor Interrupts ────────────────────────────── */

void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector) {
    /* Wait for ICR to be idle from previous sends */
    lapic_wait_icr_idle();

    /* ICR High: destination in bits 24-31 */
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24);

    /* ICR Low: vector + Fixed delivery + Physical + Edge + Assert + No shorthand */
    lapic_write(LAPIC_REG_ICR_LOW,
                (uint32_t)vector |
                LAPIC_ICR_FIXED |
                LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_EDGE |
                LAPIC_ICR_ASSERT |
                LAPIC_ICR_DEST_NONE);
}

void lapic_send_ipi_all(uint8_t vector) {
    /* Wait for ICR to be idle */
    lapic_wait_icr_idle();

    /* ICR High does not matter with shorthand "All Excluding Self" */
    lapic_write(LAPIC_REG_ICR_HIGH, 0);

    /* ICR Low: vector + Fixed + Edge + Assert + All Excluding Self */
    lapic_write(LAPIC_REG_ICR_LOW,
                (uint32_t)vector |
                LAPIC_ICR_FIXED |
                LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_EDGE |
                LAPIC_ICR_ASSERT |
                LAPIC_ICR_DEST_ALL_EX);
}

/* ── Timer LAPIC ────────────────────────────────────────────────── */

void lapic_timer_init(uint32_t frequency) {
    if (!lapic_enabled) {
        LOG_WARN("LAPIC timer: LAPIC not initialized, skipping");
        return;
    }

    if (lapic_ticks_per_sec == 0) {
        LOG_ERROR("LAPIC timer: calibration failed (0 ticks/sec), cannot init");
        return;
    }

    /* Calculate initial count for desired frequency.
     * Timer decrements with divisor 16, so:
     * initial_count = ticks_per_sec / frequency */
    uint32_t initial_count = lapic_ticks_per_sec / frequency;

    if (initial_count == 0) {
        LOG_ERROR("LAPIC timer: frequency %u Hz too high for calibrated rate", frequency);
        return;
    }

    /* Configure divisor: 16 (same as calibration) */
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);

    /* Configure LVT Timer: periodic, vector LAPIC_TIMER_VECTOR, unmasked */
    lapic_write(LAPIC_REG_TIMER_LVT,
                LAPIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC);

    /* Set initial count — this starts the timer */
    lapic_write(LAPIC_REG_TIMER_ICR, initial_count);

    LOG_OK("LAPIC timer: periodic mode at %u Hz (initial count=%u)",
           frequency, initial_count);
}

void lapic_timer_stop(void) {
    /* Mask the LVT Timer to stop interrupts */
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_TIMER_MASKED);
    /* Set initial count to 0 to stop the decrement */
    lapic_write(LAPIC_REG_TIMER_ICR, 0);
}

/* ── Timer handler (called from isr_stub_253 in interrupts.asm) ── */

/* LAPIC timer tick counter since initialization */
static volatile uint64_t lapic_timer_ticks = 0;

/*
 * LAPIC timer C handler. Called from the ISR stub.
 * Increments counter and sends EOI.
 */
void lapic_timer_handler_c(void) {
    lapic_timer_ticks++;

    /* EOI to LAPIC to unblock future interrupts */
    lapic_eoi();
}

/* Returns the LAPIC timer tick count */
uint64_t lapic_timer_get_ticks(void) {
    return lapic_timer_ticks;
}
