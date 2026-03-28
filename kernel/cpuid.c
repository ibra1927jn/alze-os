/*
 * Anykernel OS — CPUID Detection
 *
 * Uses CPUID instruction to query CPU vendor string, brand name,
 * and feature flags. Reports key features for kernel use.
 */

#include "cpuid.h"
#include "kprintf.h"
#include "log.h"
#include <stdint.h>

/* ── CPUID wrapper ───────────────────────────────────────────── */

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

/* ── Public API ──────────────────────────────────────────────── */

void cpuid_detect(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Vendor string (leaf 0) */
    cpuid(0, &eax, &ebx, &ecx, &edx);
    char vendor[13];
    *(uint32_t *)&vendor[0] = ebx;
    *(uint32_t *)&vendor[4] = edx;
    *(uint32_t *)&vendor[8] = ecx;
    vendor[12] = '\0';

    uint32_t max_leaf = eax;

    LOG_INFO("CPU: %s", vendor);

    /* Feature flags (leaf 1) */
    if (max_leaf >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);

        uint32_t family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
        uint32_t model  = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
        uint32_t stepping = eax & 0xF;

        LOG_INFO("  Family %u, Model %u, Stepping %u", family, model, stepping);

        /* Report key features */
        kprintf(ANSI_CYAN "[INFO]" ANSI_RESET "  Features:");
        if (edx & (1 << 25)) kprintf(" SSE");
        if (edx & (1 << 26)) kprintf(" SSE2");
        if (ecx & (1 <<  0)) kprintf(" SSE3");
        if (ecx & (1 <<  9)) kprintf(" SSSE3");
        if (ecx & (1 << 19)) kprintf(" SSE4.1");
        if (ecx & (1 << 20)) kprintf(" SSE4.2");
        if (ecx & (1 << 28)) kprintf(" AVX");
        if (ecx & (1 << 30)) kprintf(" RDRAND");
        if (edx & (1 <<  4)) kprintf(" TSC");
        if (edx & (1 <<  5)) kprintf(" MSR");
        if (edx & (1 << 12)) kprintf(" MTRR");
        if (edx & (1 << 13)) kprintf(" PGE");
        kprintf("\n");
    }

    /* Extended features (leaf 0x80000001) — NX bit */
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000001) {
        cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
        if (edx & (1 << 20)) {
            LOG_OK("  NX (No-Execute) bit supported");
        }
        if (edx & (1 << 29)) {
            LOG_OK("  Long Mode (x86_64) supported");
        }
    }

    /* Brand string (leaves 0x80000002-4) */
    if (eax >= 0x80000004) {
        char brand[49];
        uint32_t *b = (uint32_t *)brand;
        cpuid(0x80000002, &b[0], &b[1], &b[2], &b[3]);
        cpuid(0x80000003, &b[4], &b[5], &b[6], &b[7]);
        cpuid(0x80000004, &b[8], &b[9], &b[10], &b[11]);
        brand[48] = '\0';

        /* Skip leading spaces */
        char *p = brand;
        while (*p == ' ') p++;
        LOG_INFO("  Brand: %s", p);
    }
}
