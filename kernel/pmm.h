/*
 * Anykernel OS v2.1 — Physical Memory Manager (PMM)
 *
 * Buddy Allocator for physical page frames.
 *
 * Design:
 *   - Single global allocator, PFN-indexed page array
 *   - MAX_ORDER = 10 → blocks from 4KB (order 0) to 4MB (order 10)
 *   - struct page = 32 bytes per page frame
 *   - Holes in physical memory are marked PAGE_RESERVED
 *   - Free lists are doubly-linked for O(1) insert/remove
 *   - Buddy finding via XOR: buddy_pfn = pfn ^ (1 << order)
 *
 * Bootstrap:
 *   pmm_init() steals memory from the largest Usable region
 *   to allocate the page array, then feeds remaining pages
 *   into the buddy system.
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

/* ── Constants ────────────────────────────────────────────────── */

#define PMM_MAX_ORDER    10     /* 2^10 = 1024 pages = 4MB max block */
#define PMM_NUM_ORDERS   (PMM_MAX_ORDER + 1)

/* Page flags */
#define PAGE_FREE        0x00
#define PAGE_USED        0x01
#define PAGE_RESERVED    0x02   /* Holes, MMIO, kernel, etc. — never allocable */
#define PAGE_BUDDY_HEAD  0x04   /* Head of a free buddy block */

/* ── struct page — 32 bytes ───────────────────────────────────── */

struct page {
    struct page *next;          /* 8B — free list forward link */
    struct page *prev;          /* 8B — free list backward link */
    uint32_t    order;          /* 4B — buddy order (0..MAX_ORDER) */
    uint32_t    flags;          /* 4B — PAGE_FREE / PAGE_USED / PAGE_RESERVED */
    uint32_t    ref_count;      /* 4B — reference counter for COW / shared pages */
    uint32_t    _pad;           /* 4B — padding to maintain 32-byte alignment */
};                              /* Total: 32 bytes */

/* ── Free list head (one per order) ───────────────────────────── */

struct free_area {
    struct page *head;          /* First page in this free list */
    uint64_t    count;          /* Number of free blocks at this order */
};

/* ── PMM state ────────────────────────────────────────────────── */

struct pmm_state {
    struct page     *pages;         /* Page array (indexed by PFN) */
    uint64_t         total_pfns;    /* Total PFNs covered */
    uint64_t         max_phys_addr; /* Highest physical address */
    struct free_area free_lists[PMM_NUM_ORDERS];
    uint64_t         free_pages;    /* Total free pages */
    uint64_t         used_pages;    /* Total allocated pages */
    uint64_t         reserved_pages;/* Pages in holes/MMIO */
    uint64_t         peak_used;     /* High water mark for used pages */
};

/* ── API ──────────────────────────────────────────────────────── */

/*
 * Initialize the PMM from Limine's memory map.
 * This bootstraps the page array and populates the buddy free lists.
 *
 * When compiled for userspace testing, call pmm_init_test() instead.
 */
#ifndef PMM_USERSPACE_TEST
struct limine_memmap_response;  /* forward decl */
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm);
#endif

/*
 * Allocate 2^order contiguous page frames.
 * Returns physical address, or 0 on failure.
 */
uint64_t pmm_alloc_pages(uint32_t order);

/*
 * Allocate 2^order contiguous ZEROED page frames.
 * Returns physical address, or 0 on failure.
 * Use this for page tables, new process stacks, etc.
 */
uint64_t pmm_alloc_pages_zero(uint32_t order);

/*
 * Free 2^order contiguous page frames starting at phys_addr.
 * Coalesces with buddy if possible.
 */
void pmm_free_pages(uint64_t phys_addr, uint32_t order);

/* Diagnostics */
uint64_t pmm_free_count(void);
uint64_t pmm_used_count(void);
uint64_t pmm_peak_used(void);
void     pmm_dump_stats(void);

/*
 * Reference counting for COW (Copy-On-Write) and shared pages.
 * phys_addr must be page-aligned. All operations are atomic-safe
 * (called under pmm_lock internally).
 *
 * pmm_ref_inc: Increment ref count. Called when a page is shared
 *              (e.g. fork → COW marks page read-only, both processes ref it).
 * pmm_ref_dec: Decrement ref count. Returns the new count.
 *              When count reaches 0, the caller can free the page.
 * pmm_ref_get: Read current ref count (for diagnostics / COW decisions).
 */
void     pmm_ref_inc(uint64_t phys_addr);
uint32_t pmm_ref_dec(uint64_t phys_addr);
uint32_t pmm_ref_get(uint64_t phys_addr);

/* ── Userspace test interface ─────────────────────────────────── */

#ifdef PMM_USERSPACE_TEST
/*
 * Initialize PMM with a simulated memory region.
 * base_phys: simulated physical base address
 * size:      size in bytes
 * page_array: pre-allocated array of struct page
 * total_pfns: size of page_array
 */
void pmm_init_test(uint64_t base_phys, uint64_t size,
                   struct page *page_array, uint64_t total_pfns);
#endif

#endif /* PMM_H */
