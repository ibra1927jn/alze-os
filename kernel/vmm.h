/*
 * Anykernel OS — Virtual Memory Manager (x86_64 4-Level Paging)
 *
 * Manages the kernel's page tables (PML4 → PDPT → PD → PT).
 * After vmm_init(), the kernel runs on its own page tables
 * instead of Limine's.
 */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>

/* ── Page Table Entry flags ──────────────────────────────────── */

#define PTE_PRESENT   (1UL << 0)
#define PTE_WRITE     (1UL << 1)
#define PTE_USER      (1UL << 2)
#define PTE_PWT       (1UL << 3)   /* Write-Through */
#define PTE_PCD       (1UL << 4)   /* Cache Disable */
#define PTE_ACCESSED  (1UL << 5)
#define PTE_DIRTY     (1UL << 6)
#define PTE_HUGE      (1UL << 7)   /* 2MB page (in PD level) */
#define PTE_GLOBAL    (1UL << 8)   /* Keep in TLB on CR3 switch */
#define PTE_NX        (1UL << 63)  /* No-Execute (requires EFER.NXE) */

/* Mask to extract physical address from a PTE */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000UL

/* Convenience flag combos */
#define VMM_FLAGS_KERNEL_RX   (PTE_PRESENT | PTE_GLOBAL)
#define VMM_FLAGS_KERNEL_RO   (PTE_PRESENT | PTE_GLOBAL | PTE_NX)
#define VMM_FLAGS_KERNEL_RW   (PTE_PRESENT | PTE_WRITE | PTE_GLOBAL | PTE_NX)
#define VMM_FLAGS_HHDM        (PTE_PRESENT | PTE_WRITE | PTE_GLOBAL | PTE_NX)

/* ── Paging structure indices ────────────────────────────────── */

#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)

/* ── API ─────────────────────────────────────────────────────── */

/*
 * Build our own PML4, map kernel + HHDM, switch CR3.
 * Must be called AFTER pmm_init().
 */
void vmm_init(void);

/*
 * Map a single 4KB page: virt → phys with given flags.
 * Allocates intermediate tables (PDPT, PD, PT) as needed.
 * Flushes TLB for the virtual address.
 */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/*
 * Unmap a single 4KB page. Flushes TLB.
 * Does NOT free intermediate tables.
 */
void vmm_unmap_page(uint64_t virt);

/*
 * Walk our page tables and return the physical address
 * for a given virtual address. Returns 0 if not mapped.
 */
uint64_t vmm_virt_to_phys(uint64_t virt);

/*
 * Map a contiguous range of pages: [virt, virt+size) → [phys, phys+size).
 * size must be page-aligned.
 */
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

/*
 * Map a contiguous range using 2MB huge pages for efficiency.
 * Both virt and phys must be 2MB-aligned. size must be 2MB-aligned.
 */
void vmm_map_range_huge(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

/*
 * Diagnostic: print summary of all mapped regions.
 */
void vmm_dump_tables(void);

/*
 * Return number of page table pages allocated by VMM.
 */
uint64_t vmm_tables_count(void);

/*
 * W^X Enforcement (OpenBSD + macOS Hardened Runtime).
 * Audit all mapped pages: no page should be both Writable AND Executable.
 * Returns the number of violations found.
 */
uint32_t vmm_audit_wx(void);

#endif /* VMM_H */
