/*
 * Anykernel OS — PIC (8259A) + PIT Timer Implementation
 *
 * PIC remapping: IRQ 0-7 → vectors 0x20-0x27
 *                IRQ 8-15 → vectors 0x28-0x2F
 *
 * PIT: Channel 0, rate generator mode, ~100 Hz default.
 */

#include "pic.h"
#include "errno.h"
#include "io.h"
#include "kprintf.h"
#include "log.h"
#include <stdint.h>

/* ── PIC ports ───────────────────────────────────────────────── */

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

/* ICW1: initialization + ICW4 needed */
#define ICW1_INIT  0x11
/* ICW4: 8086 mode */
#define ICW4_8086  0x01
/* OCW2: End of Interrupt */
#define PIC_EOI    0x20

/* ICW3: Cascade wiring */
#define PIC_CASCADE_IRQ2      0x04   /* Master: slave is on IRQ2 (bit 2) */
#define PIC_SLAVE_CASCADE_ID  0x02   /* Slave: cascade identity = 2      */

/* Mask all IRQs */
#define PIC_ALL_IRQS_MASKED   0xFF

/* PIT command byte fields (bits: channel[7:6] access[5:4] mode[3:1] bcd[0]) */
#define PIT_CMD_CH0_RATE_GEN  0x34   /* Ch0, lo/hi byte, mode 2 (rate generator) */
#define PIT_CMD_CH0_ONESHOT   0x30   /* Ch0, lo/hi byte, mode 0 (one-shot)       */

/* PIT 16-bit counter maximum value */
#define PIT_MAX_COUNT         65535  /* 16-bit max (~55ms at 1.193MHz)            */
#define PIT_MAX_COUNT_LO      0xFF   /* Low byte of max count for stop mode       */
#define PIT_MAX_COUNT_HI      0xFF   /* High byte of max count for stop mode      */

/* ── PIC initialization ─────────────────────────────────────── */

void pic_init(void) {
    /* ICW1: Start initialization sequence (cascade mode, ICW4 needed) */
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();

    /* ICW2: Vector offset */
    outb(PIC1_DATA, PIC_IRQ_BASE_MASTER); io_wait();  /* Master: IRQ 0-7 → 0x20-0x27 */
    outb(PIC2_DATA, PIC_IRQ_BASE_SLAVE);  io_wait();  /* Slave:  IRQ 8-15 → 0x28-0x2F */

    /* ICW3: Cascade wiring */
    outb(PIC1_DATA, PIC_CASCADE_IRQ2);     io_wait();  /* Master: slave on IRQ2 */
    outb(PIC2_DATA, PIC_SLAVE_CASCADE_ID); io_wait();  /* Slave: identity = 2   */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask ALL IRQs initially — we unmask individually as needed */
    outb(PIC1_DATA, PIC_ALL_IRQS_MASKED);
    outb(PIC2_DATA, PIC_ALL_IRQS_MASKED);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);  /* Slave PIC */
    }
    outb(PIC1_CMD, PIC_EOI);      /* Always send to master */
}

void pic_unmask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t mask = inb(port);
    mask &= ~(1 << irq);
    outb(port, mask);
}

void pic_mask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t mask = inb(port);
    mask |= (1 << irq);
    outb(port, mask);
}

/* ── PIT Timer ───────────────────────────────────────────────── */

#define PIT_CHANNEL0   0x40
#define PIT_CMD        0x43

static volatile uint64_t pit_ticks = 0;
static uint32_t pit_freq = 0;
static volatile int pit_oneshot_mode = 0;   /* 0=periodic, 1=oneshot, 2=stopped */
static volatile uint64_t pit_idle_count = 0; /* Ticks spent idle (no PIT) */

/* Timer callback storage (forward decl for pit_tick) */
static struct {
    timer_callback_fn fn;
    uint32_t interval;
    uint32_t counter;
} timer_cbs[MAX_TIMER_CALLBACKS];

static int timer_cb_count = 0;

void pit_init(uint32_t frequency_hz) {
    pit_freq = frequency_hz;
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQ / frequency_hz);

    /* Channel 0, Access: lobyte/hibyte, Mode 2 (rate generator) */
    outb(PIT_CMD, PIT_CMD_CH0_RATE_GEN);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    pit_oneshot_mode = 0;
}

/*
 * ── Tickless Timer — Linux NO_HZ + macOS Timer Coalescing ──────
 *
 * PIT Mode 0 = Interrupt on Terminal Count (one-shot).
 * After the countdown reaches 0, OUT goes high and stays there.
 * No more interrupts until reprogrammed.
 *
 * This lets the CPU sleep until the next scheduled event
 * instead of waking 100x/sec doing nothing.
 */
void pit_set_oneshot(uint32_t ticks) {
    if (ticks == 0) ticks = 1;
    /* Cap at 16-bit max (65535 = ~55ms at 1.193MHz) */
    if (ticks > PIT_MAX_COUNT) ticks = PIT_MAX_COUNT;

    /* Channel 0, Access: lobyte/hibyte, Mode 0 (one-shot) */
    outb(PIT_CMD, PIT_CMD_CH0_ONESHOT);
    outb(PIT_CHANNEL0, (uint8_t)(ticks & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((ticks >> 8) & 0xFF));
    pit_oneshot_mode = 1;
}

void pit_stop(void) {
    /* Program PIT with max count in one-shot mode — effectively stopped.
     * The CPU will only wake on keyboard/external IRQs. */
    outb(PIT_CMD, PIT_CMD_CH0_ONESHOT);
    outb(PIT_CHANNEL0, PIT_MAX_COUNT_LO);
    outb(PIT_CHANNEL0, PIT_MAX_COUNT_HI);
    pit_oneshot_mode = 2;
}

int pit_is_tickless(void) {
    return pit_oneshot_mode;
}

uint64_t pit_idle_ticks(void) {
    return pit_idle_count;
}

/* Resume periodic mode (called when exiting idle) */
static void pit_resume_periodic(void) {
    if (pit_oneshot_mode != 0 && pit_freq > 0) {
        pit_init(pit_freq);
    }
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

/*
 * Called from the IRQ0 handler (isr_stub_32) via irq_handler_c.
 */
void pit_tick(void) {
    pit_ticks++;

    /* If we were in one-shot/stopped mode, resume periodic */
    if (pit_oneshot_mode != 0) {
        pit_idle_count++;
        pit_resume_periodic();
    }

    /* Fire timer callbacks */
    for (int i = 0; i < timer_cb_count; i++) {
        timer_cbs[i].counter++;
        if (timer_cbs[i].counter >= timer_cbs[i].interval) {
            timer_cbs[i].counter = 0;
            timer_cbs[i].fn();
        }
    }
}

/* ── Dynamic IRQ handlers ────────────────────────────────────── */

static irq_handler_fn irq_handlers[16] = {0};

irq_handler_fn irq_register(uint8_t irq, irq_handler_fn handler) {
    if (irq >= 16) return (irq_handler_fn)0;
    irq_handler_fn prev = irq_handlers[irq];
    irq_handlers[irq] = handler;
    return prev;
}

/* Called from idt.c irq_handler_c for non-timer/keyboard IRQs */
void irq_dispatch(uint8_t irq) {
    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq](irq);
    }
}

/* ── Timer callbacks ────────────────────────────────────────── */

int timer_register(timer_callback_fn fn, uint32_t interval_ticks) {
    if (timer_cb_count >= MAX_TIMER_CALLBACKS) return -ENOSPC;
    timer_cbs[timer_cb_count].fn = fn;
    timer_cbs[timer_cb_count].interval = interval_ticks;
    timer_cbs[timer_cb_count].counter = 0;
    timer_cb_count++;
    return 0;
}
