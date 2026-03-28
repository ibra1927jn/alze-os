/*
 * Anykernel OS v2.1 — UART COM1 Serial Driver
 * Port-Mapped I/O at base 0x3F8
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Initialize COM1 UART at 115200 baud, 8N1 */
void uart_init(void);

/* Send a single character (blocks until THR is empty) */
void uart_putc(char c);

/* Send a null-terminated string (converts \n to \r\n) */
void uart_puts(const char *s);

/* Print a 64-bit value as hexadecimal */
void uart_put_hex(uint64_t val);

/* Check if data is available in the receive buffer */
int uart_data_ready(void);

/* Read a single character (returns 0 if no data available) */
char uart_getc(void);

#endif /* UART_H */
