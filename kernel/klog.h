/*
 * Anykernel OS — Kernel Log Ring Buffer
 *
 * Circular buffer storing the last N log messages for
 * post-mortem debugging. Independent of UART/console.
 */

#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

/* Store a message in the ring buffer */
void klog_write(const char *msg);

/* Store a character (used by kprintf hook) */
void klog_putchar(char c);

/* Dump the ring buffer contents to kprintf */
void klog_dump(void);

/* Get buffer pointer and size for external access */
const char *klog_get_buffer(uint32_t *out_size, uint32_t *out_head);

#endif /* KLOG_H */
