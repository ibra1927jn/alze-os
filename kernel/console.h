/*
 * Anykernel OS — Framebuffer Text Console
 *
 * Renders text using a built-in 8x16 bitmap font on the
 * Limine-provided framebuffer. Supports scrolling.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

/* Initialize console from Limine framebuffer */
void console_init(void *fb_addr, uint64_t width, uint64_t height,
                  uint64_t pitch, uint16_t bpp);

/* Write a character to the console */
void console_putchar(char c);

/* Write a string to the console */
void console_puts(const char *s);

/* Clear the console */
void console_clear(void);

/* Check if console is available */
int console_available(void);

#endif /* CONSOLE_H */
