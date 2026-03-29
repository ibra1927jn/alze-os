/*
 * Anykernel OS v2.1 — Buddy Allocator (PMM Implementation)
 *
 * The buddy system manages physical memory in power-of-2 blocks:
 *   Order 0  = 4KB   (1 page)
 *   Order 1  = 8KB   (2 pages)
 *   ...
 *   Order 10 = 4MB   (1024 pages)
 *
 * Allocation: Find smallest free block >= requested size.
 *   If too large, split it in half repeatedly until exact fit.
 *
 * Freeing: Return block, then check if its "buddy" (the other half
 *   of the pair) is also free. If so, merge them into a larger block.
 *   Repeat upward until buddy is not free or MAX_ORDER reached.
 *
 * Buddy finding: buddy_pfn = pfn XOR (1 << order)
 *   This works because buddy pairs are always aligned to 2^(order+1).
 *
 * Portable: compiled for both kernel (with PHYS2VIRT) and userspace testing.
 */

#include "pmm.h"

/* ── Platform abstraction ─────────────────────────────────────── */

#ifdef PMM_USERSPACE_TEST
    #include <stdio.h>
    #include <string.h>
    #define PMM_LOG(fmt, ...)  printf("[PMM] " fmt "\n", ##__VA_ARGS__)
    #define PMM_ASSERT(cond)   do { if (!(cond)) { \
        fprintf(stderr, "PMM ASSERT FAILED: %s (%s:%d)\n", \
                #cond, __FILE__, __LINE__); \
        __builtin_trap(); \
    } } while(0)
#else
    #include "kprintf.h"
    #include "log.h"
    #include "panic.h"
    #include "memory.h"
    #include "string.h"
    #include "spinlock.h"
    #include "limine.h"
    #define PMM_LOG(fmt, ...)  LOG_INFO(fmt, ##__VA_ARGS__)
    #define PMM_ASSERT(cond)   KASSERT(cond)

    /* We only need the lock in kernel mode */
    static spinlock_t pmm_lock = SPINLOCK_INIT;
#endif

/* Page size constants (must match memory.h) */
#ifndef PAGE_SIZE
#define PAGE_SIZE  4096UL
#define PAGE_SHIFT 12
#endif

/* ── Global PMM state ─────────────────────────────────────────── */

static struct pmm_state pmm;

/* ── Free list operations (doubly-linked, O(1)) ───────────────── */

static void list_add(struct free_area *area, struct page *pg) {
    pg->prev = NULL;
    pg->next = area->head;
    if (area->head) {
        area->head->prev = pg;
    }
    area->head = pg;
    area->count++;
}

static void list_remove(struct free_area *area, struct page *pg) {
    if (pg->prev) {
        pg->prev->next = pg->next;
    } else {
        area->head = pg->next;
    }
    if (pg->next) {
        pg->next->prev = pg->prev;
    }
    pg->prev = NULL;
    pg->next = NULL;
    area->count--;
}

/* ── PFN ↔ page conversion ────────────────────────────────────── */

static inline uint64_t page_to_pfn(struct page *pg) {
    return (uint64_t)(pg - pmm.pages);
}

static inline struct page *pfn_to_page(uint64_t pfn) {
    PMM_ASSERT(pfn < pmm.total_pfns);
    return &pmm.pages[pfn];
}

static inline uint64_t pfn_to_phys(uint64_t pfn) {
    return pfn << PAGE_SHIFT;
}

static inline uint64_t phys_to_pfn(uint64_t phys) {
    return phys >> PAGE_SHIFT;
}

/* ── Buddy math ───────────────────────────────────────────────── */

/*
 * Find the buddy's PFN. For a block of order N at pfn P:
 *   buddy = P XOR (1 << N)
 *
 * Example (order 0, page size = 4KB):
 *   PFN 0 buddy = 0 ^ 1 = 1
 *   PFN 1 buddy = 1 ^ 1 = 0
 *   PFN 2 buddy = 2 ^ 1 = 3
 *
 * Example (order 1, block = 2 pages):
 *   PFN 0 buddy = 0 ^ 2 = 2
 *   PFN 2 buddy = 2 ^ 2 = 0
 */
static inline uint64_t buddy_pfn(uint64_t pfn, uint32_t order) {
    return pfn ^ (1UL << order);
}

/* Check if a page is free and at the specific order (for coalescing) */
static inline int is_buddy_free(uint64_t bpfn, uint32_t order) {
    if (bpfn >= pmm.total_pfns) return 0;
    struct page *buddy = pfn_to_page(bpfn);
    return (buddy->flags & PAGE_BUDDY_HEAD) &&
           (buddy->order == order);
}

/* ── Core: Allocate ───────────────────────────────────────────── */

uint64_t pmm_alloc_pages(uint32_t order) {
    PMM_ASSERT(order <= PMM_MAX_ORDER);

#ifndef PMM_USERSPACE_TEST
    uint64_t irq_flags;
    spin_lock_irqsave(&pmm_lock, &irq_flags);
#endif

    /* Find the smallest free block that can satisfy this request */
    uint32_t current_order = order;
    while (current_order <= PMM_MAX_ORDER) {
        if (pmm.free_lists[current_order].head != NULL) {
            break;
        }
        current_order++;
    }

    if (current_order > PMM_MAX_ORDER) {
        /* No block large enough */
#ifndef PMM_USERSPACE_TEST
        spin_unlock_irqrestore(&pmm_lock, irq_flags);
#endif
        return 0;
    }

    /* Remove the block from its free list */
    struct page *block = pmm.free_lists[current_order].head;
    list_remove(&pmm.free_lists[current_order], block);
    block->flags &= ~PAGE_BUDDY_HEAD;
    pmm.free_pages -= (1UL << current_order);  /* Entire block leaves free pool */

    /* Split down to the requested order */
    while (current_order > order) {
        current_order--;

        /* The second half becomes a new free buddy */
        uint64_t pfn = page_to_pfn(block);
        uint64_t split_pfn = pfn + (1UL << current_order);

        struct page *split_buddy = pfn_to_page(split_pfn);
        split_buddy->order = current_order;
        split_buddy->flags = PAGE_FREE | PAGE_BUDDY_HEAD;
        list_add(&pmm.free_lists[current_order], split_buddy);
        pmm.free_pages += (1UL << current_order);  /* Split half returns to free */
    }

    /* Mark the block as used, ref_count = 1 (primer propietario) */
    block->order = order;
    block->flags = PAGE_USED;
    block->ref_count = 1;
    pmm.used_pages += (1UL << order);
    if (pmm.used_pages > pmm.peak_used) pmm.peak_used = pmm.used_pages;

#ifndef PMM_USERSPACE_TEST
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
#endif

    return pfn_to_phys(page_to_pfn(block));
}

/* ── Core: Allocate Zeroed ────────────────────────────────────── */

uint64_t pmm_alloc_pages_zero(uint32_t order) {
    uint64_t addr = pmm_alloc_pages(order);
    if (addr == 0) return 0;

    uint64_t size = (1UL << order) * PAGE_SIZE;

#ifdef PMM_USERSPACE_TEST
    /* In test mode we can't actually zero the "physical" memory */
    (void)size;
#else
    /* Zero through HHDM virtual mapping */
    extern uint64_t hhdm_offset;
    memset((void *)(addr + hhdm_offset), 0, size);
#endif

    return addr;
}

/* ── Core: Free + Coalesce ────────────────────────────────────── */

void pmm_free_pages(uint64_t phys_addr, uint32_t order) {
    PMM_ASSERT(order <= PMM_MAX_ORDER);
    PMM_ASSERT((phys_addr & (PAGE_SIZE - 1)) == 0);  /* Must be page-aligned */

    uint64_t pfn = phys_to_pfn(phys_addr);
    PMM_ASSERT(pfn < pmm.total_pfns);

    struct page *pg = pfn_to_page(pfn);
    PMM_ASSERT(pg->flags & PAGE_USED);  /* Must be allocated */

#ifndef PMM_USERSPACE_TEST
    uint64_t irq_flags;
    spin_lock_irqsave(&pmm_lock, &irq_flags);
#endif

    pg->flags = PAGE_FREE | PAGE_BUDDY_HEAD;
    pg->order = order;
    pg->ref_count = 0;  /* Pagina libre no tiene referencias */
    pmm.free_pages += (1UL << order);
    pmm.used_pages -= (1UL << order);

    /* Try to coalesce with buddy, climbing up orders */
    while (order < PMM_MAX_ORDER) {
        uint64_t bpfn = buddy_pfn(pfn, order);

        if (!is_buddy_free(bpfn, order)) {
            break;  /* Buddy is not free or wrong order — stop */
        }

        /* Remove buddy from its free list (we're merging) */
        struct page *buddy = pfn_to_page(bpfn);
        list_remove(&pmm.free_lists[order], buddy);
        buddy->flags &= ~PAGE_BUDDY_HEAD;

        /* The merged block starts at the lower PFN */
        if (bpfn < pfn) {
            pfn = bpfn;
            pg = buddy;
        }

        /* Climb to the next order */
        order++;
        pg->order = order;
        pg->flags = PAGE_FREE | PAGE_BUDDY_HEAD;
    }

    /* Insert merged block into the appropriate free list */
    list_add(&pmm.free_lists[order], pg);

#ifndef PMM_USERSPACE_TEST
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
#endif
}

/* ── Diagnostics ──────────────────────────────────────────────── */

uint64_t pmm_free_count(void) {
    return pmm.free_pages;
}

uint64_t pmm_used_count(void) {
    return pmm.used_pages;
}

uint64_t pmm_peak_used(void) {
    return pmm.peak_used;
}

void pmm_dump_stats(void) {
    PMM_LOG("PMM Stats: %lu free, %lu used, %lu reserved (of %lu total PFNs)",
            pmm.free_pages, pmm.used_pages, pmm.reserved_pages, pmm.total_pfns);

    for (uint32_t i = 0; i <= PMM_MAX_ORDER; i++) {
        if (pmm.free_lists[i].count > 0) {
            PMM_LOG("  Order %2u (%6lu KB): %lu blocks free",
                    i, (1UL << i) * PAGE_SIZE / 1024, pmm.free_lists[i].count);
        }
    }
}

/* ── Reference counting (COW / shared pages) ─────────────────── */

/*
 * Incrementa el contador de referencias de una pagina fisica.
 * Se usa al compartir paginas entre procesos (fork + COW).
 */
void pmm_ref_inc(uint64_t phys_addr) {
    PMM_ASSERT((phys_addr & (PAGE_SIZE - 1)) == 0);
    uint64_t pfn = phys_to_pfn(phys_addr);
    PMM_ASSERT(pfn < pmm.total_pfns);

#ifndef PMM_USERSPACE_TEST
    uint64_t irq_flags;
    spin_lock_irqsave(&pmm_lock, &irq_flags);
#endif

    struct page *pg = pfn_to_page(pfn);
    pg->ref_count++;

#ifndef PMM_USERSPACE_TEST
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
#endif
}

/*
 * Decrementa el contador de referencias. Retorna el nuevo valor.
 * Cuando llega a 0, el caller puede liberar la pagina con pmm_free_pages.
 */
uint32_t pmm_ref_dec(uint64_t phys_addr) {
    PMM_ASSERT((phys_addr & (PAGE_SIZE - 1)) == 0);
    uint64_t pfn = phys_to_pfn(phys_addr);
    PMM_ASSERT(pfn < pmm.total_pfns);

#ifndef PMM_USERSPACE_TEST
    uint64_t irq_flags;
    spin_lock_irqsave(&pmm_lock, &irq_flags);
#endif

    struct page *pg = pfn_to_page(pfn);
    PMM_ASSERT(pg->ref_count > 0);
    pg->ref_count--;
    uint32_t new_count = pg->ref_count;

#ifndef PMM_USERSPACE_TEST
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
#endif

    return new_count;
}

/*
 * Lee el contador de referencias actual (para diagnostico o decisiones COW).
 */
uint32_t pmm_ref_get(uint64_t phys_addr) {
    PMM_ASSERT((phys_addr & (PAGE_SIZE - 1)) == 0);
    uint64_t pfn = phys_to_pfn(phys_addr);
    PMM_ASSERT(pfn < pmm.total_pfns);

    /* Proteger lectura con lock, igual que ref_inc/ref_dec */
#ifndef PMM_USERSPACE_TEST
    uint64_t irq_flags;
    spin_lock_irqsave(&pmm_lock, &irq_flags);
#endif

    struct page *pg = pfn_to_page(pfn);
    uint32_t count = pg->ref_count;

#ifndef PMM_USERSPACE_TEST
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
#endif

    return count;
}

/* ── Userspace test init ──────────────────────────────────────── */

#ifdef PMM_USERSPACE_TEST

void pmm_init_test(uint64_t base_phys, uint64_t size,
                   struct page *page_array, uint64_t total_pfns) {
    /* Zero state */
    memset(&pmm, 0, sizeof(pmm));
    pmm.pages = page_array;
    pmm.total_pfns = total_pfns;
    pmm.max_phys_addr = total_pfns * PAGE_SIZE;

    /* Mark everything as reserved initially */
    for (uint64_t i = 0; i < total_pfns; i++) {
        pmm.pages[i].flags = PAGE_RESERVED;
        pmm.pages[i].order = 0;
        pmm.pages[i].next = NULL;
        pmm.pages[i].prev = NULL;
        pmm.pages[i].ref_count = 0;
        pmm.pages[i]._pad = 0;
    }
    pmm.reserved_pages = total_pfns;

    /* Free the simulated usable region */
    uint64_t start_pfn = phys_to_pfn(base_phys);
    uint64_t end_pfn   = phys_to_pfn(base_phys + size);

    if (end_pfn > total_pfns) end_pfn = total_pfns;

    /* Feed pages in MAX_ORDER-aligned chunks for efficient coalescing */
    for (uint64_t pfn = start_pfn; pfn < end_pfn; ) {
        /* Find the largest order block we can free starting at pfn */
        uint32_t order = 0;
        while (order < PMM_MAX_ORDER) {
            uint64_t block_pages = 1UL << (order + 1);
            /* Block must be naturally aligned and fit within the region */
            if ((pfn & (block_pages - 1)) != 0) break;
            if (pfn + block_pages > end_pfn) break;
            order++;
        }

        /* Mark these pages as used (so pmm_free_pages can un-use them) */
        uint64_t block_pages = 1UL << order;
        for (uint64_t i = 0; i < block_pages; i++) {
            pmm.pages[pfn + i].flags = PAGE_USED;
        }
        pmm.reserved_pages -= block_pages;
        pmm.used_pages += block_pages;

        /* Free them into the buddy system */
        pmm_free_pages(pfn_to_phys(pfn), order);

        pfn += block_pages;
    }

    PMM_LOG("Test init: %lu pages total, %lu free, %lu reserved",
            total_pfns, pmm.free_pages, pmm.reserved_pages);
}

#endif /* PMM_USERSPACE_TEST */

/* ── Kernel init (uses Limine memory map) ─────────────────────── */

#ifndef PMM_USERSPACE_TEST

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm) {
    PMM_ASSERT(memmap != NULL);

    memset(&pmm, 0, sizeof(pmm));

    /* Pass 1: Find highest usable physical address to determine array size.
     * We only need to track pages in regions the kernel can actually use.
     * Reserved/MMIO regions at high addresses (e.g., 0xFD00000000) would
     * create a massive, mostly-empty page array, wasting all our RAM. */
    uint64_t max_addr = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t end = e->base + e->length;
        if (end > max_addr) max_addr = end;
    }

    pmm.max_phys_addr = max_addr;
    pmm.total_pfns = max_addr / PAGE_SIZE;

    uint64_t metadata_size = pmm.total_pfns * sizeof(struct page);
    uint64_t metadata_pages = (metadata_size + PAGE_SIZE - 1) / PAGE_SIZE;

    PMM_LOG("Total PFNs: %lu, metadata: %lu KB (%lu pages)",
            pmm.total_pfns, metadata_size / 1024, metadata_pages);

    /* Pass 2: Find largest Usable region to steal metadata from */
    uint64_t steal_base = 0;
    uint64_t steal_size = 0;
    uint64_t steal_idx = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length > steal_size) {
            steal_base = e->base;
            steal_size = e->length;
            steal_idx = i;
        }
    }

    PMM_ASSERT(steal_size >= metadata_size);
    PMM_LOG("Stealing %lu KB from region %lu (base 0x%lx) for page array",
            metadata_size / 1024, steal_idx, steal_base);

    /* Place page array at the beginning of the stolen region */
    pmm.pages = (struct page *)((uint64_t)steal_base + hhdm);

    /* Zero the page array */
    memset(pmm.pages, 0, metadata_size);

    /* Mark ALL pages as reserved initially */
    for (uint64_t i = 0; i < pmm.total_pfns; i++) {
        pmm.pages[i].flags = PAGE_RESERVED;
    }
    pmm.reserved_pages = pmm.total_pfns;

    /* Pass 3: Feed Usable regions into the buddy system */
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t region_base = e->base;
        uint64_t region_end  = e->base + e->length;

        /* Skip metadata region we stole */
        if (i == steal_idx) {
            region_base += metadata_pages * PAGE_SIZE;
            if (region_base >= region_end) continue;
        }

        /* Align to page boundaries */
        uint64_t start_pfn = (region_base + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t end_pfn   = region_end / PAGE_SIZE;

        if (start_pfn >= end_pfn) continue;
        if (end_pfn > pmm.total_pfns) end_pfn = pmm.total_pfns;

        /* Feed pages using largest possible buddy blocks */
        for (uint64_t pfn = start_pfn; pfn < end_pfn; ) {
            uint32_t order = 0;
            while (order < PMM_MAX_ORDER) {
                uint64_t block_pages = 1UL << (order + 1);
                if ((pfn & (block_pages - 1)) != 0) break;
                if (pfn + block_pages > end_pfn) break;
                order++;
            }

            uint64_t block_pages = 1UL << order;
            for (uint64_t j = 0; j < block_pages; j++) {
                pmm.pages[pfn + j].flags = PAGE_USED;
            }
            pmm.reserved_pages -= block_pages;
            pmm.used_pages += block_pages;

            pmm_free_pages(pfn_to_phys(pfn), order);

            pfn += block_pages;
        }
    }

    PMM_LOG("Initialized: %lu free, %lu reserved, %lu total",
            pmm.free_pages, pmm.reserved_pages, pmm.total_pfns);
}

#endif /* !PMM_USERSPACE_TEST */
