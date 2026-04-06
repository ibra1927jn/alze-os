/*
 * Anykernel OS — Low-level CPU Intrinsics
 *
 * Shared inline helpers for x86_64 CPU operations.
 */

#ifndef CPU_H
#define CPU_H

#include <stdint.h>

/* Read Time Stamp Counter (RDTSC) — returns 64-bit cycle count */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif /* CPU_H */
