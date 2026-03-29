/*
 * Anykernel OS v2.1 — Freestanding kprintf Implementation
 *
 * Zero-allocation printf for kernel diagnostics.
 * All output goes through uart_putc().
 *
 * Supported:
 *   %s          — string (with optional width: %20s, %-20s)
 *   %d / %ld    — signed decimal
 *   %u / %lu    — unsigned decimal
 *   %x / %lx    — hex lowercase
 *   %X / %lX    — hex uppercase
 *   %p          — pointer (0x + 16-digit hex)
 *   %c          — character
 *   %%          — literal %
 *   %0Nx        — zero-padded to N digits (e.g. %016lx, %08x, %02x)
 *   %-Ns        — left-aligned string in N-width field
 *   %ll*        — same as %l* (both treat as 64-bit)
 */

#include "kprintf.h"
#include "uart.h"
#include "console.h"
#include "klog.h"
#include "pic.h"      /* pit_get_ticks() for rate limiter */
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Internal helpers ─────────────────────────────────────────── */

static void kprint_char(char c) {
    if (c == '\n') {
        uart_putc('\r');
    }
    uart_putc(c);
    console_putchar(c);  /* Also render to framebuffer (no-op if not init'd) */
    klog_putchar(c);     /* Also store in ring buffer for post-mortem */
}

static void kprint_string_padded(const char *s, int width, bool left_align) {
    if (!s) s = "(null)";

    /* Measure string length */
    int len = 0;
    const char *p = s;
    while (*p++) len++;

    /* Right-pad (default): print spaces first, then string */
    if (!left_align) {
        for (int i = len; i < width; i++) kprint_char(' ');
    }

    /* Print the string */
    p = s;
    while (*p) kprint_char(*p++);

    /* Left-align: print trailing spaces */
    if (left_align) {
        for (int i = len; i < width; i++) kprint_char(' ');
    }
}

static void kprint_unsigned(uint64_t val, int base, int width, char pad, bool uppercase) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_upper : digits_lower;

    char buf[20]; /* max uint64 decimal = 20 digits */
    int i = 0;

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }

    /* Padding */
    while (i < width) {
        kprint_char(pad);
        width--;
    }

    /* Print digits in reverse */
    while (i > 0) {
        kprint_char(buf[--i]);
    }
}

static void kprint_signed(int64_t val, int width, char pad) {
    if (val < 0) {
        kprint_char('-');
        if (width > 0) width--;
        kprint_unsigned((uint64_t)(-val), 10, width, pad, false);
    } else {
        kprint_unsigned((uint64_t)val, 10, width, pad, false);
    }
}

/* ── kprintf ──────────────────────────────────────────────────── */

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            kprint_char(*fmt++);
            continue;
        }

        fmt++; /* skip '%' */

        /* Parse flags */
        bool left_align = false;
        char pad = ' ';

        if (*fmt == '-') {
            left_align = true;
            fmt++;
        } else if (*fmt == '0') {
            pad = '0';
            fmt++;
        }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse length modifier: l or ll */
        bool is_long = false;
        if (*fmt == 'l') {
            is_long = true;
            fmt++;
            if (*fmt == 'l') fmt++;  /* skip second 'l' in %lld/%llx */
        }

        /* Format specifier */
        switch (*fmt) {
        case 's':
            kprint_string_padded(va_arg(args, const char *), width, left_align);
            break;

        case 'd':
            if (is_long) {
                kprint_signed(va_arg(args, int64_t), width, pad);
            } else {
                kprint_signed(va_arg(args, int), width, pad);
            }
            break;

        case 'u':
            if (is_long) {
                kprint_unsigned(va_arg(args, uint64_t), 10, width, pad, false);
            } else {
                kprint_unsigned(va_arg(args, unsigned int), 10, width, pad, false);
            }
            break;

        case 'x':
            if (is_long) {
                kprint_unsigned(va_arg(args, uint64_t), 16, width, pad, false);
            } else {
                kprint_unsigned(va_arg(args, unsigned int), 16, width, pad, false);
            }
            break;

        case 'X':
            if (is_long) {
                kprint_unsigned(va_arg(args, uint64_t), 16, width, pad, true);
            } else {
                kprint_unsigned(va_arg(args, unsigned int), 16, width, pad, true);
            }
            break;

        case 'p':
            kprint_string_padded("0x", 0, false);
            kprint_unsigned((uint64_t)va_arg(args, void *), 16, 16, '0', false);
            break;

        case 'c':
            kprint_char((char)va_arg(args, int));
            break;

        case '%':
            kprint_char('%');
            break;

        default:
            /* Unknown specifier — print as-is */
            kprint_char('%');
            kprint_char(*fmt);
            break;
        }

        fmt++;
    }

    va_end(args);
}

/* ── ksnprintf — format into a buffer ────────────────────────── */

struct snprintf_ctx {
    char   *buf;
    size_t  size;
    size_t  pos;
};

static void snprint_char(struct snprintf_ctx *ctx, char c) {
    if (ctx->pos + 1 < ctx->size) {
        ctx->buf[ctx->pos] = c;
    }
    ctx->pos++;
}

static void snprint_string(struct snprintf_ctx *ctx, const char *s) {
    if (!s) s = "(null)";
    while (*s) snprint_char(ctx, *s++);
}

static void snprint_unsigned(struct snprintf_ctx *ctx, uint64_t val,
                              int base, int width, char pad, bool uppercase) {
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *digits = uppercase ? up : lo;
    char tmp[20];
    int i = 0;

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) { tmp[i++] = digits[val % base]; val /= base; }
    }
    while (i < width) { snprint_char(ctx, pad); width--; }
    while (i > 0) snprint_char(ctx, tmp[--i]);
}

static void snprint_signed(struct snprintf_ctx *ctx, int64_t val,
                            int width, char pad) {
    if (val < 0) {
        snprint_char(ctx, '-');
        if (width > 0) width--;
        snprint_unsigned(ctx, (uint64_t)(-val), 10, width, pad, false);
    } else {
        snprint_unsigned(ctx, (uint64_t)val, 10, width, pad, false);
    }
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    struct snprintf_ctx ctx = { .buf = buf, .size = size, .pos = 0 };

    while (*fmt) {
        if (*fmt != '%') {
            snprint_char(&ctx, *fmt++);
            continue;
        }
        fmt++;

        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        else if (*fmt == '-') { fmt++; } /* skip left-align for now */

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        bool is_long = false;
        if (*fmt == 'l') { is_long = true; fmt++; if (*fmt == 'l') fmt++; }

        switch (*fmt) {
        case 's': snprint_string(&ctx, va_arg(args, const char *)); break;
        case 'd':
            if (is_long) snprint_signed(&ctx, va_arg(args, int64_t), width, pad);
            else snprint_signed(&ctx, va_arg(args, int), width, pad);
            break;
        case 'u':
            if (is_long) snprint_unsigned(&ctx, va_arg(args, uint64_t), 10, width, pad, false);
            else snprint_unsigned(&ctx, va_arg(args, unsigned int), 10, width, pad, false);
            break;
        case 'x':
            if (is_long) snprint_unsigned(&ctx, va_arg(args, uint64_t), 16, width, pad, false);
            else snprint_unsigned(&ctx, va_arg(args, unsigned int), 16, width, pad, false);
            break;
        case 'X':
            if (is_long) snprint_unsigned(&ctx, va_arg(args, uint64_t), 16, width, pad, true);
            else snprint_unsigned(&ctx, va_arg(args, unsigned int), 16, width, pad, true);
            break;
        case 'p':
            snprint_string(&ctx, "0x");
            snprint_unsigned(&ctx, (uint64_t)va_arg(args, void *), 16, 16, '0', false);
            break;
        case 'c': snprint_char(&ctx, (char)va_arg(args, int)); break;
        case '%': snprint_char(&ctx, '%'); break;
        default: snprint_char(&ctx, '%'); snprint_char(&ctx, *fmt); break;
        }
        fmt++;
    }

    /* NUL-terminate */
    if (size > 0) {
        buf[ctx.pos < size ? ctx.pos : size - 1] = '\0';
    }
    return (int)ctx.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = kvsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

/* ── Rate limiter (Linux printk_ratelimit pattern) ───────────── */

int kprintf_rl_allow(struct kprintf_ratelimit *rl) {
    uint64_t now = pit_get_ticks();

    /* New window? Reset counter */
    if (now - rl->last_tick >= rl->interval_ticks) {
        if (rl->count > rl->burst) {
            /* Report how many were suppressed */
            kprintf("[WARN]  kprintf_ratelimit: %u messages suppressed\n",
                    rl->count - rl->burst);
        }
        rl->last_tick = now;
        rl->count = 0;
    }

    rl->count++;
    return (rl->count <= rl->burst);
}
