/*
 * Anykernel OS — PS/2 Keyboard Driver
 *
 * Reads scancodes from the PS/2 keyboard controller (port 0x60).
 * Converts Set 1 scancodes to ASCII via a lookup table.
 * Maintains a small ring buffer for key events.
 */

#ifndef KB_H
#define KB_H

#include <stdint.h>
#include <stdbool.h>

/* Initialize keyboard (unmask IRQ1) */
void kb_init(void);

/* Called from IRQ1 handler — reads scancode and buffers it */
void kb_irq_handler(void);

/* Get next ASCII character from buffer. Returns 0 if empty. */
char kb_getchar(void);

/* Check if there are characters available */
bool kb_has_input(void);

#endif /* KB_H */
