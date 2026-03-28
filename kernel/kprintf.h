/*
 * Anykernel OS v2.1 — Freestanding kprintf
 *
 * Lightweight kernel printf using <stdarg.h> (compiler-provided).
 * Triple output: UART serial + framebuffer console + klog ring buffer.
 * No heap allocation, no floating point.
 *
 * Supported format specifiers:
 *   %s   — string
 *   %d   — signed decimal (int)
 *   %u   — unsigned decimal (uint32_t / uint64_t with 'l')
 *   %x   — hexadecimal (lowercase, no prefix)
 *   %p   — pointer (0x-prefixed hex, always 16 digits)
 *   %c   — single character
 *   %%   — literal '%'
 *   %ld / %lu / %lx — 64-bit variants
 */

#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* Print formatted output to UART + console + klog */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Format into a buffer (like snprintf). Returns chars written (excl NUL). */
int ksnprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* va_list variant of ksnprintf */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list args);

/*
 * Rate-limited kprintf — Linux printk_ratelimit pattern.
 * Prevents log spam from hot paths or misbehaving drivers.
 *
 * Usage:
 *   static struct kprintf_ratelimit rl = KPRINTF_RL_INIT(100, 5);
 *   if (kprintf_rl_allow(&rl))
 *       kprintf("repeated event\n");
 */
struct kprintf_ratelimit {
    uint64_t interval_ticks;  /* Minimum ticks between prints */
    uint32_t burst;           /* Max prints per interval */
    uint32_t count;           /* Current count in window */
    uint64_t last_tick;       /* Tick of last window start */
};

#define KPRINTF_RL_INIT(interval, max_burst) \
    { .interval_ticks = (interval), .burst = (max_burst), .count = 0, .last_tick = 0 }

/* Returns true if this print should be allowed */
int kprintf_rl_allow(struct kprintf_ratelimit *rl);

#endif /* KPRINTF_H */
