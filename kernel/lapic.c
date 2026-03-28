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

/* ── Estado interno ─────────────────────────────────────────────── */

/* Flag: 1 si el LAPIC fue inicializado correctamente */
static volatile int lapic_enabled = 0;

/* Ticks del LAPIC timer por segundo (calculado durante calibracion) */
static uint32_t lapic_ticks_per_sec = 0;

/* Frecuencia configurada del timer (Hz) */
static uint32_t lapic_timer_freq = 0;

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
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));

    /* Verificar bit 11: APIC habilitado globalmente */
    if (!(lo & (1 << 11))) {
        LOG_WARN("LAPIC: APIC Global Enable bit not set in IA32_APIC_BASE MSR");
        /* Intentar habilitarlo */
        lo |= (1 << 11);
        asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0x1B));
        LOG_INFO("LAPIC: enabled APIC via IA32_APIC_BASE MSR");
    }

    /* Extraer la base fisica (bits 12..35, alineada a 4KB) */
    uint64_t base = ((uint64_t)hi << 32) | (lo & 0xFFFFF000UL);
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
    #define CALIBRATION_PIT_TICKS   11932   /* ~10ms a 1.193182 MHz */

    /* Deshabilitar speaker, habilitar gate para channel 2 */
    uint8_t gate = inb(0x61);
    gate = (gate & 0xFD) | 0x01;   /* bit 0=gate on, bit 1=speaker off */
    outb(0x61, gate);

    /* PIT channel 2, mode 0 (interrupt on terminal count), lo/hi byte */
    outb(0x43, 0xB0);              /* 10_11_000_0: ch2, lobyte/hibyte, mode 0 */
    outb(0x42, CALIBRATION_PIT_TICKS & 0xFF);
    outb(0x42, (CALIBRATION_PIT_TICKS >> 8) & 0xFF);

    /* Reiniciar el gate para comenzar la cuenta del PIT channel 2 */
    gate = inb(0x61);
    outb(0x61, gate & 0xFE);       /* Gate off */
    outb(0x61, gate | 0x01);       /* Gate on — comienza la cuenta */

    /* Arrancar LAPIC timer con cuenta maxima */
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    /* Esperar a que PIT channel 2 llegue a 0.
     * El bit 5 del port 0x61 indica el output de channel 2.
     * Cuando la cuenta termina, el output pasa a 1 (mode 0). */
    while (!(inb(0x61) & 0x20)) {
        asm volatile("pause");
    }

    /* Leer cuanto decremento el LAPIC timer */
    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CCR);
    uint32_t elapsed = 0xFFFFFFFF - remaining;

    /* Detener el LAPIC timer */
    lapic_write(LAPIC_REG_TIMER_LVT, LAPIC_TIMER_MASKED);

    /* Escalar a ticks por segundo.
     * elapsed ticks ocurrieron en ~10ms → ticks/sec = elapsed * 100 */
    uint32_t ticks_per_sec = elapsed * 100;

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
    uint32_t max_lvt = ((version >> 16) & 0xFF) + 1;
    LOG_INFO("LAPIC: version 0x%x, max LVT entries: %u", version & 0xFF, max_lvt);

    /* Limpiar TPR: aceptar todas las interrupciones (prioridad 0) */
    lapic_write(LAPIC_REG_TPR, 0);

    /* Configurar Destination Format Register: flat model (todos los bits en 1) */
    lapic_write(LAPIC_REG_DFR, 0xFFFFFFFF);

    /* Configurar Logical Destination Register: bit para esta CPU.
     * En flat model, LDR bits 24-31 son un bitmask de 8 bits.
     * Si el LAPIC ID >= 8, usamos bit 0 como fallback seguro. */
    uint32_t id = lapic_get_id();
    uint32_t ldr_bit = (id < 8) ? (1U << id) : 1U;
    lapic_write(LAPIC_REG_LDR, ldr_bit << 24);

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

    /* Enviar EOI por si queda alguna interrupcion pendiente */
    lapic_eoi();

    /* Calibrar el timer (necesario antes de lapic_timer_init) */
    lapic_ticks_per_sec = lapic_calibrate_timer();

    /* Marcar como habilitado */
    lapic_enabled = 1;

    uint32_t lapic_id = lapic_get_id();
    LOG_OK("LAPIC: initialized (ID=%u, base=0x%lx, spurious=0x%x)",
           lapic_id, (uint64_t)LAPIC_BASE_PHYS, LAPIC_SPURIOUS_VECTOR);
}

/* ── Queries ────────────────────────────────────────────────────── */

uint32_t lapic_get_id(void) {
    /* El LAPIC ID esta en bits 24-31 del registro ID */
    return (lapic_read(LAPIC_REG_ID) >> 24) & 0xFF;
}

int lapic_is_enabled(void) {
    return lapic_enabled;
}

/* ── End-Of-Interrupt ───────────────────────────────────────────── */

void lapic_eoi(void) {
    /* Escribir 0 al registro EOI senala fin de interrupcion.
     * Cualquier valor funciona pero 0 es la convencion. */
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

    lapic_timer_freq = frequency;

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
