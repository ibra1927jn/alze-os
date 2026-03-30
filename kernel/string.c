/*
 * Anykernel OS v2.1 — Freestanding String/Memory Functions
 *
 * These are mandatory for freestanding C. The compiler assumes they
 * exist and emits implicit calls for struct init, array copies, etc.
 *
 * Uses x86_64 `rep stosb` / `rep movsb` for hardware-accelerated
 * bulk memory operations (ERMS — Enhanced REP MOVSB/STOSB on modern CPUs).
 */

#include "string.h"
#include <stdint.h>

void *memset(void *dest, int val, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    uint8_t v = (uint8_t)val;

    /*
     * Optimization: use rep stosq (8 bytes/cycle) for bulk,
     * then rep stosb for the remaining 0-7 bytes.
     * This is ~8x faster than pure rep stosb for large fills.
     */
    if (n >= 8) {
        /* Broadcast byte to 8-byte qword: 0xAA -> 0xAAAAAAAAAAAAAAAA */
        uint64_t qval = v;
        qval |= qval << 8;
        qval |= qval << 16;
        qval |= qval << 32;

        size_t qwords = n / 8;
        size_t remain = n % 8;

        asm volatile(
            "rep stosq"
            : "+D"(d), "+c"(qwords)
            : "a"(qval)
            : "memory"
        );

        /* Fill remaining bytes */
        if (remain) {
            asm volatile(
                "rep stosb"
                : "+D"(d), "+c"(remain)
                : "a"(v)
                : "memory"
            );
        }
    } else {
        /* Small fill: just use byte-by-byte */
        asm volatile(
            "rep stosb"
            : "+D"(d), "+c"(n)
            : "a"(v)
            : "memory"
        );
    }

    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    /*
     * Optimization: use rep movsq (8 bytes/cycle) for bulk,
     * then rep movsb for the remaining 0-7 bytes.
     */
    if (n >= 8) {
        size_t qwords = n / 8;
        size_t remain = n % 8;

        asm volatile(
            "rep movsq"
            : "+D"(d), "+S"(s), "+c"(qwords)
            :
            : "memory"
        );

        if (remain) {
            asm volatile(
                "rep movsb"
                : "+D"(d), "+S"(s), "+c"(remain)
                :
                : "memory"
            );
        }
    } else {
        asm volatile(
            "rep movsb"
            : "+D"(d), "+S"(s), "+c"(n)
            :
            : "memory"
        );
    }

    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0) {
        return dest;
    }

    if (d < s || d >= s + n) {
        /* No overlap or dest is before src — forward copy is safe */
        return memcpy(dest, src, n);
    }

    /* Overlap with dest > src — copy backwards.
     * Use rep movsq with STD (direction flag) for bulk, then residual bytes. */
    if (n >= 8) {
        size_t qwords = n / 8;
        size_t remain = n % 8;

        /* Copy residual bytes first (at the end of the buffer) */
        d += n;
        s += n;
        for (size_t i = 0; i < remain; i++) {
            *(--d) = *(--s);
        }

        /* rep movsq with STD: rdi/rsi point to the last qword */
        d -= 8;  /* Point to the last qword to copy */
        s -= 8;
        asm volatile(
            "std\n\t"
            "rep movsq\n\t"
            "cld"
            : "+D"(d), "+S"(s), "+c"(qwords)
            :
            : "memory"
        );
    } else {
        d += n;
        s += n;
        while (n--) {
            *(--d) = *(--s);
        }
    }

    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }

    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : 0;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) return (void *)&p[i];
    }
    return 0;
}
