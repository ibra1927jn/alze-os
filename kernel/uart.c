/*
 * Anykernel OS v2.1 — UART COM1 Serial Driver
 *
 * Implements UART communication via Port-Mapped I/O.
 * Uses polling (no interrupts) for simplicity in Sprint 1.
 *
 * Register map (base 0x3F8):
 *   +0  THR  Transmitter Holding Register (write) / RBR Receiver Buffer (read)
 *   +0  DLL  Divisor Latch Low  (when DLAB=1)
 *   +1  DLH  Divisor Latch High (when DLAB=1)
 *   +1  IER  Interrupt Enable Register (when DLAB=0)
 *   +2  FCR  FIFO Control Register (write)
 *   +3  LCR  Line Control Register
 *   +4  MCR  Modem Control Register
 *   +5  LSR  Line Status Register
 */

#include "uart.h"
#include "io.h"

/* ── COM1 register definitions ────────────────────────────────── */

#define COM1_BASE   0x3F8

#define REG_THR     (COM1_BASE + 0)   /* Transmit Holding Register      */
#define REG_DLL     (COM1_BASE + 0)   /* Divisor Latch Low  (DLAB=1)    */
#define REG_DLH     (COM1_BASE + 1)   /* Divisor Latch High (DLAB=1)    */
#define REG_IER     (COM1_BASE + 1)   /* Interrupt Enable Register      */
#define REG_FCR     (COM1_BASE + 2)   /* FIFO Control Register          */
#define REG_LCR     (COM1_BASE + 3)   /* Line Control Register          */
#define REG_MCR     (COM1_BASE + 4)   /* Modem Control Register         */
#define REG_LSR     (COM1_BASE + 5)   /* Line Status Register           */

#define LCR_DLAB    0x80              /* Divisor Latch Access Bit       */
#define LCR_8N1     0x03              /* 8 data bits, no parity, 1 stop */
#define LSR_THRE    0x20              /* Bit 5: THR Empty               */
#define FCR_ENABLE  0x07              /* Enable + clear both FIFOs      */
#define MCR_OUT2    0x08              /* OUT2: enable IRQ routing        */
#define MCR_RTS_DTR 0x03              /* RTS + DTR active               */

/*
 * Baud rate divisor:
 *   UART clock = 115200 * 16 = 1,843,200 Hz (standard oscillator)
 *   divisor = 1,843,200 / (16 * baud_rate)
 *   For 115200 baud: divisor = 1
 */
#define BAUD_DIVISOR 1

/* Timeout for TX polling — prevents deadlock on missing hardware */
#define UART_TIMEOUT 100000

/* Hardware presence flag */
static int uart_available = 0;

/* ── Implementation ───────────────────────────────────────────── */

void uart_init(void) {
    /* Detect if COM1 hardware exists:
     * Write a known value to the scratch register and read it back.
     * If the value matches, hardware is present. */
    outb(COM1_BASE + 7, 0xAE);  /* Scratch register at base+7 */
    if (inb(COM1_BASE + 7) != 0xAE) {
        uart_available = 0;
        return;  /* No serial hardware — all uart_put* become no-ops */
    }
    uart_available = 1;

    /* 1. Disable all interrupts */
    outb(REG_IER, 0x00);

    /* 2. Enable DLAB to set baud rate divisor */
    outb(REG_LCR, LCR_DLAB);

    /* 3. Set divisor (115200 baud) */
    outb(REG_DLL, (BAUD_DIVISOR >> 0) & 0xFF);  /* Low byte  */
    outb(REG_DLH, (BAUD_DIVISOR >> 8) & 0xFF);  /* High byte */

    /* 4. Configure line: 8 data bits, no parity, 1 stop bit (8N1) */
    /*    This also clears DLAB */
    outb(REG_LCR, LCR_8N1);

    /* 5. Enable and clear FIFOs, set 14-byte threshold */
    outb(REG_FCR, FCR_ENABLE);

    /* 6. Set modem control: DTR + RTS + OUT2 (required for IRQ delivery) */
    outb(REG_MCR, MCR_RTS_DTR | MCR_OUT2);
}

void uart_putc(char c) {
    if (!uart_available) return;

    /* Poll LSR bit 5 (THRE) with timeout — prevents deadlock */
    uint32_t timeout = UART_TIMEOUT;
    while (!(inb(REG_LSR) & LSR_THRE)) {
        if (--timeout == 0) return;  /* Fail safe and silent */
    }

    /* Send the byte */
    outb(REG_THR, c);
}

void uart_puts(const char *s) {
    while (*s) {
        /* Convert bare \n to \r\n for proper serial terminal display */
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s);
        s++;
    }
}

void uart_put_hex(uint64_t val) {
    static const char hex[] = "0123456789abcdef";

    uart_puts("0x");

    /* Print 16 hex digits (64-bit) — skip leading zeros */
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        if (nibble != 0 || started || i == 0) {
            uart_putc(hex[nibble]);
            started = 1;
        }
    }
}

/* ── UART Receive (polling) ──────────────────────────────────── */

#define LSR_DR 0x01  /* Bit 0: Data Ready in RBR */

int uart_data_ready(void) {
    return inb(REG_LSR) & LSR_DR;
}

char uart_getc(void) {
    if (!uart_data_ready()) return 0;
    return (char)inb(COM1_BASE);  /* Read from RBR (same port as THR) */
}

