/*
 * Anykernel OS — Local APIC (LAPIC) Driver Implementation
 *
 * Implementacion completa del controlador LAPIC para x86_64:
 *   - MMIO read/write sobre la base estandar 0xFEE00000
 *   - Inicializacion: habilitar SVR, limpiar TPR, configurar LVT
 *   - EOI: liberar linea de interrupcion
 *   - IPI: enviar interrupciones entre CPUs (punto a punto y broadcast)
 *   - Timer: calibrado contra PIT, modo periodico configurable
 *
 * El LAPIC se accede via HHDM (Higher Half Direct Map) ya que
 * sus registros estan mapeados fisicamente en 0xFEE00000.
 *
 * Referencia: Intel SDM Vol. 3A, Chapter 10
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

/* ── Estado interno ─────────────────────────────────────────────── */

/* Flag: 1 si el LAPIC fue inicializado correctamente */
static volatile int lapic_enabled = 0;

/* Ticks del LAPIC timer por segundo (calculado durante calibracion) */
static uint32_t lapic_ticks_per_sec = 0;


/* ── MMIO: acceso a registros LAPIC ─────────────────────────────── */

uint32_t lapic_read(uint32_t reg) {
    /* Cada registro LAPIC es un uint32_t en la base + offset.
     * Se accede via HHDM para obtener la direccion virtual. */
    volatile uint32_t *addr = (volatile uint32_t *)PHYS2VIRT(LAPIC_BASE_PHYS + reg);
    return *addr;
}

void lapic_write(uint32_t reg, uint32_t val) {
    volatile uint32_t *addr = (volatile uint32_t *)PHYS2VIRT(LAPIC_BASE_PHYS + reg);
    *addr = val;
}

/* ── Esperar a que el ICR este libre ────────────────────────────── */

/*
 * El LAPIC puede tardar en enviar un IPI. El bit 12 (Delivery Status)
 * del ICR Low indica si hay un envio pendiente. Esperamos hasta que
 * sea 0 (idle) antes de escribir un nuevo IPI.
 */
static void lapic_wait_icr_idle(void) {
    while (lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_PENDING) {
        asm volatile("pause");
    }
}

/* ── Verificar soporte LAPIC via MSR ────────────────────────────── */

/*
 * Lee el MSR IA32_APIC_BASE (0x1B) para verificar que el LAPIC
 * esta habilitado en hardware y obtener la base fisica.
 * Bit 11: APIC Global Enable.
 * Bits 12-35: Base Address (deberia ser 0xFEE00000).
 */
static int lapic_check_msr(void) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_IA32_APIC_BASE));

    /* Verificar bit 11: APIC habilitado globalmente */
    if (!(lo & APIC_GLOBAL_ENABLE)) {
        LOG_WARN("LAPIC: APIC Global Enable bit not set in IA32_APIC_BASE MSR");
        /* Intentar habilitarlo */
        lo |= APIC_GLOBAL_ENABLE;
        asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(MSR_IA32_APIC_BASE));
        LOG_INFO("LAPIC: enabled APIC via IA32_APIC_BASE MSR");
    }

    /* Extraer la base fisica (bits 12..35, alineada a 4KB) */
    uint64_t base = ((uint64_t)hi << 32) | (lo & APIC_BASE_ADDR_MASK);
    if (base != LAPIC_BASE_PHYS) {
        LOG_WARN("LAPIC: base address 0x%lx differs from standard 0x%lx",
                 base, (uint64_t)LAPIC_BASE_PHYS);
        /* Usamos la estandar de todas formas, la HHDM ya la mapea */
    }

    return 1;
}

/* ── Calibracion del timer LAPIC contra el PIT ──────────────────── */

/*
 * Estrategia: usar el PIT como referencia de tiempo conocida.
 * 1. Configurar PIT channel 2 en one-shot para ~10ms (11932 ticks a 1.193182 MHz)
 * 2. Arrancar el LAPIC timer con initial count = 0xFFFFFFFF
 * 3. Esperar a que PIT channel 2 expire
 * 4. Leer cuanto decremento el LAPIC timer
 * 5. Escalar a ticks por segundo
 *
 * Precision: ±1% es suficiente para scheduling; HPET o TSC
 * pueden mejorar esto si se necesita mejor precision.
 */
static uint32_t lapic_calibrate_timer(void) {
    /* Configurar LAPIC timer: divisor 16, one-shot, mascarado */
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_TIMER_MASKED);

    /*
     * Usar PIT channel 2 como referencia.
     * PIT channel 2 esta conectado al speaker gate (port 0x61).
     * Lo configuramos en one-shot mode 0 con un periodo de ~10ms.
     *
     * PIT freq = 1193182 Hz → ~10ms = 11932 ticks
     */
    #define CALIBRATION_MS          10
    #define CALIBRATION_PIT_TICKS   ((uint16_t)(PIT_BASE_FREQ * CALIBRATION_MS / 1000))

    /* Deshabilitar speaker, habilitar gate para channel 2 */
    uint8_t gate = inb(PORT_SPEAKER_GATE);
    gate = (gate & SPEAKER_OFF_MASK) | SPEAKER_GATE_ON;
    outb(PORT_SPEAKER_GATE, gate);

    /* PIT channel 2, mode 0 (interrupt on terminal count), lo/hi byte */
    outb(PORT_PIT_CMD, PIT_CMD_CH2_ONESHOT);
    outb(PORT_PIT_CH2_DATA, CALIBRATION_PIT_TICKS & 0xFF);
    outb(PORT_PIT_CH2_DATA, (CALIBRATION_PIT_TICKS >> 8) & 0xFF);

    /* Reiniciar el gate para comenzar la cuenta del PIT channel 2 */
    gate = inb(PORT_SPEAKER_GATE);
    outb(PORT_SPEAKER_GATE, gate & ~SPEAKER_GATE_ON);  /* Gate off */
    outb(PORT_SPEAKER_GATE, gate | SPEAKER_GATE_ON);   /* Gate on — comienza */

    /* Arrancar LAPIC timer con cuenta maxima */
    lapic_write(LAPIC_REG_TIMER_ICR, LAPIC_TIMER_MAX_COUNT);

    /* Esperar a que PIT channel 2 llegue a 0.
     * El bit 5 del port 0x61 indica el output de channel 2.
     * Cuando la cuenta termina, el output pasa a 1 (mode 0).
     * Timeout para evitar hang si el PIT no responde. */
    uint32_t pit_timeout = PIT_CALIBRATION_TIMEOUT;
    while (!(inb(PORT_SPEAKER_GATE) & PIT_CH2_OUTPUT_BIT) && --pit_timeout > 0) {
        asm volatile("pause");
    }

    /* Leer cuanto decremento el LAPIC timer */
    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    uint32_t elapsed = LAPIC_TIMER_MAX_COUNT - remaining;

    /* Si el PIT no respondio, usar un fallback razonable */
    if (pit_timeout == 0) {
        kprintf("[LAPIC] PIT calibration timeout, using fallback\n");
        elapsed = LAPIC_FALLBACK_TICKS;
    }

    /* Detener el LAPIC timer */
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_TIMER_MASKED);

    /* Escalar a ticks por segundo.
     * elapsed ticks ocurrieron en ~10ms → ticks/sec = elapsed * 100.
     * Use 64-bit multiply to avoid overflow if elapsed > ~42M ticks. */
    uint64_t ticks64 = (uint64_t)elapsed * 100;
    if (ticks64 > UINT32_MAX) ticks64 = UINT32_MAX;
    uint32_t ticks_per_sec = (uint32_t)ticks64;

    LOG_INFO("LAPIC timer: calibrated %u ticks in ~10ms → %u ticks/sec",
             elapsed, ticks_per_sec);

    return ticks_per_sec;
}

/* ── Inicializacion del LAPIC ───────────────────────────────────── */

void lapic_init(void) {
    /* Verificar y habilitar LAPIC via MSR */
    if (!lapic_check_msr()) {
        LOG_ERROR("LAPIC: failed to enable via MSR, aborting init");
        return;
    }

    /* Leer version para diagnostico */
    uint32_t version = lapic_read(LAPIC_REG_VERSION);
    uint32_t max_lvt = ((version >> LAPIC_MAX_LVT_SHIFT) & LAPIC_MAX_LVT_MASK) + 1;
    LOG_INFO("LAPIC: version 0x%x, max LVT entries: %u",
             version & LAPIC_VERSION_MASK, max_lvt);

    /* Limpiar TPR: aceptar todas las interrupciones (prioridad 0) */
    lapic_write(LAPIC_REG_TPR, 0);

    /* Configurar Destination Format Register: flat model (todos los bits en 1) */
    lapic_write(LAPIC_REG_DFR, LAPIC_DFR_FLAT_MODEL);

    /* Configurar Logical Destination Register: bit para esta CPU.
     * En flat model, LDR bits 24-31 son un bitmask de 8 bits.
     * Si el LAPIC ID >= 8, usamos bit 0 como fallback seguro. */
    uint32_t id = lapic_get_id();
    uint32_t ldr_bit = (id < LAPIC_MAX_FLAT_CPUS) ? (1U << id) : 1U;
    lapic_write(LAPIC_REG_LDR, ldr_bit << LAPIC_LDR_SHIFT);

    /* Habilitar LAPIC via SVR:
     * Bit 8 = Enable, bits 0-7 = spurious vector (0xFF) */
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    /* Limpiar Error Status Register (escribir dos veces segun Intel SDM) */
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);

    /* Mascarar LVT entries que no usamos todavia */
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
    /* Esperar a que ICR este libre de envios previos */
    lapic_wait_icr_idle();

    /* ICR High: destino en bits 24-31 */
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
    /* Esperar a que ICR este libre */
    lapic_wait_icr_idle();

    /* ICR High no importa con shorthand "All Excluding Self" */
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

    /* Calcular initial count para la frecuencia deseada.
     * El timer decrementa con divisor 16, asi que:
     * initial_count = ticks_per_sec / frequency */
    uint32_t initial_count = lapic_ticks_per_sec / frequency;

    if (initial_count == 0) {
        LOG_ERROR("LAPIC timer: frequency %u Hz too high for calibrated rate", frequency);
        return;
    }

    /* Configurar divisor: 16 (mismo que calibracion) */
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);

    /* Configurar LVT Timer: periodico, vector LAPIC_TIMER_VECTOR, sin mascara */
    lapic_write(LAPIC_REG_TIMER_LVT,
                LAPIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC);

    /* Establecer initial count — esto arranca el timer */
    lapic_write(LAPIC_REG_TIMER_ICR, initial_count);

    LOG_OK("LAPIC timer: periodic mode at %u Hz (initial count=%u)",
           frequency, initial_count);
}

void lapic_timer_stop(void) {
    /* Mascarar el LVT Timer para detener las interrupciones */
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_TIMER_MASKED);
    /* Poner initial count a 0 para detener el decremento */
    lapic_write(LAPIC_REG_TIMER_ICR, 0);
}

/* ── Timer handler (llamado desde isr_stub_253 en interrupts.asm) ── */

/* Contador de ticks del timer LAPIC desde que se inicio */
static volatile uint64_t lapic_timer_ticks = 0;

/*
 * Handler C del timer LAPIC. Llamado desde el ISR stub.
 * Incrementa el contador y envia EOI.
 * En el futuro puede integrarse con el scheduler (sched_tick).
 */
void lapic_timer_handler_c(void) {
    lapic_timer_ticks++;

    /* EOI al LAPIC para desbloquear futuras interrupciones */
    lapic_eoi();
}

/* Devuelve el numero de ticks del timer LAPIC */
uint64_t lapic_timer_get_ticks(void) {
    return lapic_timer_ticks;
}
