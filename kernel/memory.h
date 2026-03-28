/*
 * Anykernel OS v2.1 — Memory Constants & Address Translation
 *
 * Core definitions for the memory subsystem:
 *   - Page size and alignment macros (bitwise, no division)
 *   - HHDM physical↔virtual address translation
 */

#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

/* ── Page geometry ────────────────────────────────────────────── */

#define PAGE_SIZE   4096UL
#define PAGE_SHIFT  12          /* log2(PAGE_SIZE) */
#define PAGE_MASK   (~(PAGE_SIZE - 1))

/* ── Alignment macros (bitwise — no division, max speed) ──────── */

/* Round `addr` UP to the next multiple of `align` (must be power of 2) */
#define ALIGN_UP(addr, align)   \
    (((uint64_t)(addr) + (uint64_t)(align) - 1) & ~((uint64_t)(align) - 1))

/* Round `addr` DOWN to the previous multiple of `align` (must be power of 2) */
#define ALIGN_DOWN(addr, align) \
    ((uint64_t)(addr) & ~((uint64_t)(align) - 1))

/* Page-granularity shortcuts */
#define PAGE_ALIGN_UP(addr)     ALIGN_UP(addr, PAGE_SIZE)
#define PAGE_ALIGN_DOWN(addr)   ALIGN_DOWN(addr, PAGE_SIZE)

/* Check if `addr` is page-aligned */
#define IS_PAGE_ALIGNED(addr)   (((uint64_t)(addr) & (PAGE_SIZE - 1)) == 0)

/* Convert between addresses and page frame numbers */
#define ADDR_TO_PFN(addr)       ((uint64_t)(addr) >> PAGE_SHIFT)
#define PFN_TO_ADDR(pfn)        ((uint64_t)(pfn) << PAGE_SHIFT)

/* ── HHDM: Physical ↔ Virtual address translation ────────────── */

/*
 * The Higher Half Direct Map (HHDM) is a linear mapping of ALL physical
 * memory into the kernel's virtual address space. Limine creates it
 * and tells us the offset (typically 0xFFFF800000000000).
 *
 * To access physical address 0x1000 from kernel C code:
 *   void *ptr = PHYS2VIRT(0x1000);
 *
 * Rule: NEVER dereference a raw physical address in kernel code.
 *       ALWAYS go through PHYS2VIRT first.
 */

/* Global HHDM offset — set by main.c from Limine's response */
extern uint64_t hhdm_offset;

#define PHYS2VIRT(phys)   ((void *)((uint64_t)(phys) + hhdm_offset))
#define VIRT2PHYS(virt)   ((uint64_t)(virt) - hhdm_offset)

/* ── Size helper macros ───────────────────────────────────────── */

#define KB(x)  ((uint64_t)(x) * 1024UL)
#define MB(x)  ((uint64_t)(x) * 1024UL * 1024UL)
#define GB(x)  ((uint64_t)(x) * 1024UL * 1024UL * 1024UL)

#endif /* MEMORY_H */
