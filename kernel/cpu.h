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

/* Execute CPUID instruction for a given leaf */
static inline void cpuid_leaf(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                              uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

#endif /* CPU_H */
