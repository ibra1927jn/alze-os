/*
 * Anykernel OS — CPUID Detection
 *
 * Uses CPUID instruction to query CPU vendor string, brand name,
 * and feature flags. Reports key features for kernel use.
 */

#include "cpuid.h"
#include "cpu.h"
#include "kprintf.h"
#include "log.h"
#include <stdint.h>

/* ── CPUID leaf 1: family/model extraction bit fields ────────── */

#define CPUID_FAMILY_SHIFT       8
#define CPUID_FAMILY_MASK        0xF
#define CPUID_EXT_FAMILY_SHIFT   20
#define CPUID_EXT_FAMILY_MASK    0xFF
#define CPUID_MODEL_SHIFT        4
#define CPUID_MODEL_MASK         0xF
#define CPUID_EXT_MODEL_SHIFT    16
#define CPUID_EXT_MODEL_MASK     0xF
#define CPUID_STEPPING_MASK      0xF

/* CPUID leaf 1 EDX feature bits */
#define CPUID_EDX_TSC            (1 <<  4)
#define CPUID_EDX_MSR            (1 <<  5)
#define CPUID_EDX_MTRR           (1 << 12)
#define CPUID_EDX_PGE            (1 << 13)
#define CPUID_EDX_SSE            (1 << 25)
#define CPUID_EDX_SSE2           (1 << 26)

/* CPUID leaf 1 ECX feature bits */
#define CPUID_ECX_SSE3           (1 <<  0)
#define CPUID_ECX_SSSE3          (1 <<  9)
#define CPUID_ECX_SSE41          (1 << 19)
#define CPUID_ECX_SSE42          (1 << 20)
#define CPUID_ECX_AVX            (1 << 28)
#define CPUID_ECX_RDRAND         (1 << 30)

/* CPUID extended leaf 0x80000001 EDX feature bits */
#define CPUID_EXT_LEAF           0x80000000
#define CPUID_EXT_FEATURES_LEAF  0x80000001
#define CPUID_EXT_EDX_NX         (1 << 20)
#define CPUID_EXT_EDX_LM         (1 << 29)

/* CPUID brand string leaves */
#define CPUID_BRAND_LEAF_START   0x80000002
#define CPUID_BRAND_LEAF_END     0x80000004

/* ── Public API ──────────────────────────────────────────────── */

void cpuid_detect(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Vendor string (leaf 0) */
    cpuid_leaf(0, &eax, &ebx, &ecx, &edx);
    char vendor[13];
    *(uint32_t *)&vendor[0] = ebx;
    *(uint32_t *)&vendor[4] = edx;
    *(uint32_t *)&vendor[8] = ecx;
    vendor[12] = '\0';

    uint32_t max_leaf = eax;

    LOG_INFO("CPU: %s", vendor);

    /* Feature flags (leaf 1) */
    if (max_leaf >= 1) {
        cpuid_leaf(1, &eax, &ebx, &ecx, &edx);

        uint32_t family = ((eax >> CPUID_FAMILY_SHIFT) & CPUID_FAMILY_MASK)
                        + ((eax >> CPUID_EXT_FAMILY_SHIFT) & CPUID_EXT_FAMILY_MASK);
        uint32_t model  = ((eax >> CPUID_MODEL_SHIFT) & CPUID_MODEL_MASK)
                        | (((eax >> CPUID_EXT_MODEL_SHIFT) & CPUID_EXT_MODEL_MASK) << 4);
        uint32_t stepping = eax & CPUID_STEPPING_MASK;

        LOG_INFO("  Family %u, Model %u, Stepping %u", family, model, stepping);

        /* Report key features */
        kprintf(ANSI_CYAN "[INFO]" ANSI_RESET "  Features:");
        if (edx & CPUID_EDX_SSE)    kprintf(" SSE");
        if (edx & CPUID_EDX_SSE2)   kprintf(" SSE2");
        if (ecx & CPUID_ECX_SSE3)   kprintf(" SSE3");
        if (ecx & CPUID_ECX_SSSE3)  kprintf(" SSSE3");
        if (ecx & CPUID_ECX_SSE41)  kprintf(" SSE4.1");
        if (ecx & CPUID_ECX_SSE42)  kprintf(" SSE4.2");
        if (ecx & CPUID_ECX_AVX)    kprintf(" AVX");
        if (ecx & CPUID_ECX_RDRAND) kprintf(" RDRAND");
        if (edx & CPUID_EDX_TSC)    kprintf(" TSC");
        if (edx & CPUID_EDX_MSR)    kprintf(" MSR");
        if (edx & CPUID_EDX_MTRR)   kprintf(" MTRR");
        if (edx & CPUID_EDX_PGE)    kprintf(" PGE");
        kprintf("\n");
    }

    /* Extended features (leaf 0x80000001) — NX bit */
    cpuid_leaf(CPUID_EXT_LEAF, &eax, &ebx, &ecx, &edx);
    if (eax >= CPUID_EXT_FEATURES_LEAF) {
        cpuid_leaf(CPUID_EXT_FEATURES_LEAF, &eax, &ebx, &ecx, &edx);
        if (edx & CPUID_EXT_EDX_NX) {
            LOG_OK("  NX (No-Execute) bit supported");
        }
        if (edx & CPUID_EXT_EDX_LM) {
            LOG_OK("  Long Mode (x86_64) supported");
        }
    }

    /* Brand string (leaves 0x80000002-4) */
    if (eax >= CPUID_BRAND_LEAF_END) {
        char brand[49];
        uint32_t *b = (uint32_t *)brand;
        cpuid_leaf(CPUID_BRAND_LEAF_START + 0, &b[0], &b[1], &b[2], &b[3]);
        cpuid_leaf(CPUID_BRAND_LEAF_START + 1, &b[4], &b[5], &b[6], &b[7]);
        cpuid_leaf(CPUID_BRAND_LEAF_START + 2, &b[8], &b[9], &b[10], &b[11]);
        brand[48] = '\0';

        /* Skip leading spaces */
        char *p = brand;
        while (*p == ' ') p++;
        LOG_INFO("  Brand: %s", p);
    }
}
