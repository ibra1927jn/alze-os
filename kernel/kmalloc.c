/*
 * Anykernel OS v2.1 — Kernel Heap Allocator Implementation
 *
 * Slab allocator with 8 size classes:
 *
 *   Class 0: 16B   (256 slots per 4KB page)
 *   Class 1: 32B   (128 slots)
 *   Class 2: 64B   (64 slots)
 *   Class 3: 128B  (32 slots)
 *   Class 4: 256B  (16 slots)
 *   Class 5: 512B  (8 slots)
 *   Class 6: 1024B (4 slots)
 *   Class 7: 2048B (2 slots)
 *
 * Each slab page layout:
 *   [slab_header (32B)] [slot0] [slot1] [slot2] ...
 *
 * Free slots use an intrusive linked list: the first 8 bytes
 * of each free slot point to the next free slot.
 *
 * For allocations > 2048B: allocate full pages from PMM directly.
 * The size is stored in the first 8 bytes of the page (hidden header).
 */

#include "kmalloc.h"
#include "pmm.h"
#include "memory.h"
#include "string.h"
#include "spinlock.h"
#include "log.h"
#include "panic.h"

/* ── Constants ────────────────────────────────────────────────── */

#define SLAB_NUM_CLASSES  8
#define SLAB_MIN_SIZE     16
#define SLAB_MAX_SIZE     2048   /* Larger allocs go to PMM directly */

/* Poison byte: written to freed memory to detect use-after-free.
 * If you see 0xDEDEDEDE in a register dump, you freed too early. */
#define POISON_BYTE       0xDE

/* ── Slab header (at the start of each slab page) ─────────────── */

struct slab_header {
    struct slab_header *next;     /* Next slab page in this class */
    void               *free;    /* Head of free slot list */
    uint32_t           obj_size; /* Size of each slot */
    uint32_t           used;     /* Number of allocated slots */
    uint32_t           total;    /* Total slots in this slab */
    uint32_t           _pad;     /* Align to 32 bytes */
};

_Static_assert(sizeof(struct slab_header) == 32, "slab_header must be 32B");

/* ── Per-class state ──────────────────────────────────────────── */

struct slab_class {
    uint32_t            obj_size;    /* Slot size for this class */
    struct slab_header *slabs;       /* Linked list of slab pages */
    spinlock_t          lock;        /* Per-class lock (no contention across sizes) */
    uint64_t            total_allocs;
    uint64_t            total_frees;
};

static struct slab_class classes[SLAB_NUM_CLASSES];
static spinlock_t large_lock = SPINLOCK_INIT;  /* Only for > SLAB_MAX_SIZE allocs */
static uint64_t large_allocs = 0;
static uint64_t large_frees  = 0;

/*
 * ── Quarantine Ring (OpenBSD malloc + macOS libmalloc) ───────────
 *
 * Freed memory is NOT immediately returned to the free list.
 * Instead, it sits in a quarantine ring for QUARANTINE_SIZE frees,
 * giving us time to detect use-after-free.
 *
 * OpenBSD does this with delayed free + junk filling.
 * macOS libmalloc uses a "nano" quarantine for small objects.
 */
#define QUARANTINE_SIZE 32

static struct {
    void *entries[QUARANTINE_SIZE];
    int head;  /* Next write position */
    int count; /* Number of valid entries */
} quarantine = { .head = 0, .count = 0 };

static spinlock_t quarantine_lock = SPINLOCK_INIT;

/* ── Helpers ──────────────────────────────────────────────────── */

/* Find the size class index (log2 of rounded-up-to-power-of-2) */
static int size_to_class(uint64_t size) {
    if (size <= 16)   return 0;
    if (size <= 32)   return 1;
    if (size <= 64)   return 2;
    if (size <= 128)  return 3;
    if (size <= 256)  return 4;
    if (size <= 512)  return 5;
    if (size <= 1024) return 6;
    if (size <= SLAB_MAX_SIZE) return 7;
    return -1;  /* Too large for slab → page alloc */
}

/* Convert physical address to virtual via HHDM */
static inline void *phys_to_virt(uint64_t phys) {
    extern uint64_t hhdm_offset;
    return (void *)(phys + hhdm_offset);
}

static inline uint64_t virt_to_phys(void *virt) {
    extern uint64_t hhdm_offset;
    return (uint64_t)virt - hhdm_offset;
}

/* Create a new slab page for a given class */
static struct slab_header *slab_new(struct slab_class *cls) {
    /* Allocate a zeroed page from PMM */
    uint64_t phys = pmm_alloc_pages_zero(0);  /* Order 0 = 1 page */
    if (phys == 0) return NULL;

    struct slab_header *slab = (struct slab_header *)phys_to_virt(phys);

    /* Initialize header */
    slab->next = NULL;
    slab->obj_size = cls->obj_size;
    slab->used = 0;

    /* Calculate slots: skip the header at the beginning */
    uint64_t data_start = (uint64_t)(slab + 1);  /* After header */
    uint64_t page_end   = (uint64_t)slab + PAGE_SIZE;
    slab->total = (uint32_t)((page_end - data_start) / cls->obj_size);

    /* Build free list: chain all slots together */
    slab->free = NULL;
    for (uint32_t i = 0; i < slab->total; i++) {
        void *slot = (void *)(data_start + (uint64_t)i * cls->obj_size);
        *(void **)slot = slab->free;  /* Point to previous head */
        slab->free = slot;
    }

    return slab;
}

/* Find which slab owns a given pointer */
static struct slab_header *slab_from_ptr(void *ptr) {
    /* Slab header is at the page-aligned base of the pointer */
    return (struct slab_header *)((uint64_t)ptr & ~(PAGE_SIZE - 1));
}

/* ── Init ─────────────────────────────────────────────────────── */

/* Called implicitly by first kmalloc — lazy init */
static int initialized = 0;

static void kmalloc_init(void) {
    uint32_t size = SLAB_MIN_SIZE;
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        classes[i].obj_size = size;
        classes[i].slabs = NULL;
        classes[i].lock = (spinlock_t)SPINLOCK_INIT;
        classes[i].total_allocs = 0;
        classes[i].total_frees = 0;
        size *= 2;
    }
    initialized = 1;
}

/* ── kmalloc ──────────────────────────────────────────────────── */

void *kmalloc(uint64_t size) {
    if (size == 0) return NULL;

    if (!initialized) kmalloc_init();

    int cls_idx = size_to_class(size);

    if (cls_idx < 0) {
        /* Large allocation: use large_lock */
        uint64_t irq_flags;
        spin_lock_irqsave(&large_lock, &irq_flags);

        uint32_t order = 0;
        uint64_t needed = size + 8;
        while ((1UL << order) * PAGE_SIZE < needed && order < PMM_MAX_ORDER) {
            order++;
        }

        /* Verify that order does not exceed the maximum allowed */
        if (order > PMM_MAX_ORDER) {
            spin_unlock_irqrestore(&large_lock, irq_flags);
            return NULL;
        }

        uint64_t phys = pmm_alloc_pages_zero(order);
        if (phys == 0) {
            spin_unlock_irqrestore(&large_lock, irq_flags);
            return NULL;
        }

        void *virt = phys_to_virt(phys);
        *(uint64_t *)virt = order;
        large_allocs++;
        spin_unlock_irqrestore(&large_lock, irq_flags);
        return (void *)((uint64_t)virt + 8);
    }

    /* Slab allocation: use per-class lock */
    struct slab_class *cls = &classes[cls_idx];
    uint64_t irq_flags;
    spin_lock_irqsave(&cls->lock, &irq_flags);

    struct slab_header *slab = cls->slabs;
    while (slab && slab->free == NULL) {
        slab = slab->next;
    }

    if (slab == NULL) {
        slab = slab_new(cls);
        if (slab == NULL) {
            spin_unlock_irqrestore(&cls->lock, irq_flags);
            return NULL;
        }
        slab->next = cls->slabs;
        cls->slabs = slab;
    }

    void *slot = slab->free;
    slab->free = *(void **)slot;
    slab->used++;
    cls->total_allocs++;

    spin_unlock_irqrestore(&cls->lock, irq_flags);
    return slot;
}

/* Return a slot to its slab free list; free the slab page if empty.
 * Caller MUST hold classes[cls_idx].lock. */
static void slab_return_slot(void *slot, struct slab_header *slab, int cls_idx) {
    *(void **)slot = slab->free;
    slab->free = slot;
    slab->used--;
    if (slab->used == 0) {
        struct slab_header **pp = &classes[cls_idx].slabs;
        while (*pp && *pp != slab) pp = &(*pp)->next;
        if (*pp == slab) *pp = slab->next;
        memset(slab, POISON_BYTE, PAGE_SIZE);
        uint64_t phys = virt_to_phys((void *)slab);
        pmm_free_pages(phys, 0);
    }
}

/* ── kfree helpers ────────────────────────────────────────────── */

/* Free a large (> SLAB_MAX_SIZE) allocation that was page-allocated.
 * Returns true if ptr was a large allocation, false otherwise. */
static bool kfree_large(void *ptr) {
    uint64_t addr = (uint64_t)ptr;
    if (((addr - 8) & (PAGE_SIZE - 1)) != 0)
        return false;

    uint64_t irq_flags;
    spin_lock_irqsave(&large_lock, &irq_flags);
    void *base = (void *)(addr - 8);
    uint32_t order = (uint32_t)(*(uint64_t *)base);
    /* Validate that order was not corrupted before using it */
    if (order > PMM_MAX_ORDER) {
        LOG_ERROR("kmalloc: corrupted large header: order=%u", order);
        spin_unlock_irqrestore(&large_lock, irq_flags);
        return true;  /* Was large, but corrupted — do not free */
    }
    memset(base, POISON_BYTE, (1UL << order) * PAGE_SIZE);
    uint64_t phys = virt_to_phys(base);
    pmm_free_pages(phys, order);
    large_frees++;
    spin_unlock_irqrestore(&large_lock, irq_flags);
    return true;
}

/* ── kfree ────────────────────────────────────────────────────── */

void kfree(void *ptr) {
    if (ptr == NULL) return;

    /* Check if this is a large allocation (page-aligned - 8 bytes) */
    if (kfree_large(ptr)) return;

    /* ── Double-free detection (OpenBSD hardened malloc) ────────── */
    struct slab_header *slab = slab_from_ptr(ptr);
    int cls_idx = size_to_class(slab->obj_size);
    KASSERT(cls_idx >= 0);  /* Validate that obj_size is a known class */
    KASSERT(slab->used > 0);

    uint64_t irq_flags;
    spin_lock_irqsave(&classes[cls_idx].lock, &irq_flags);

    /* Check poison under lock: if post-header bytes already have
     * the poison pattern, this memory was already freed → double-free! */
    if (slab->obj_size > 8) {
        uint8_t *check = (uint8_t *)ptr + 8;
        if (check[0] == POISON_BYTE && check[1] == POISON_BYTE &&
            check[2] == POISON_BYTE && check[3] == POISON_BYTE) {
            spin_unlock_irqrestore(&classes[cls_idx].lock, irq_flags);
            LOG_ERROR("DOUBLE FREE detected at %p (class %u)",
                      ptr, slab->obj_size);
            KASSERT(0);  /* Halt */
        }
    }

    /* Poison the slot (except first 8 bytes — used for free list) */
    if (slab->obj_size > 8) {
        memset((uint8_t *)ptr + 8, POISON_BYTE, slab->obj_size - 8);
    }

    /* Quarantine: delay reuse via ring buffer.
     * Extract the old entry from the ring BEFORE inserting the new one.
     * If the old entry belongs to another class, acquire its lock. */
    void *evicted = NULL;
    {
        uint64_t q_flags;
        spin_lock_irqsave(&quarantine_lock, &q_flags);

        if (quarantine.count >= QUARANTINE_SIZE) {
            evicted = quarantine.entries[quarantine.head];
        }

        quarantine.entries[quarantine.head] = ptr;
        quarantine.head = (quarantine.head + 1) % QUARANTINE_SIZE;
        if (quarantine.count < QUARANTINE_SIZE) quarantine.count++;

        spin_unlock_irqrestore(&quarantine_lock, q_flags);
    }

    /* Return the evicted entry to its free list, with the correct lock */
    if (evicted) {
        struct slab_header *old_slab = slab_from_ptr(evicted);
        int old_cls = size_to_class(old_slab->obj_size);

        if (old_cls >= 0 && old_cls != cls_idx) {
            /* Different class: release current lock, take the other class lock. */
            spin_unlock_irqrestore(&classes[cls_idx].lock, irq_flags);

            uint64_t old_flags;
            spin_lock_irqsave(&classes[old_cls].lock, &old_flags);
            slab_return_slot(evicted, old_slab, old_cls);
            classes[old_cls].total_frees++;
            spin_unlock_irqrestore(&classes[old_cls].lock, old_flags);

            /* Re-acquire our class lock for the final total_frees update */
            spin_lock_irqsave(&classes[cls_idx].lock, &irq_flags);
        } else if (old_cls == cls_idx) {
            /* Same class: we already hold the lock */
            slab_return_slot(evicted, old_slab, cls_idx);
        }
        /* old_cls < 0 = corrupted, silently ignore */
    }

    classes[cls_idx].total_frees++;
    spin_unlock_irqrestore(&classes[cls_idx].lock, irq_flags);
}

/* ── kzmalloc ─────────────────────────────────────────────────── */

void *kzmalloc(uint64_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

/* ── kmalloc_usable_size ──────────────────────────────────────── */

uint64_t kmalloc_usable_size(void *ptr) {
    if (ptr == NULL) return 0;

    uint64_t addr = (uint64_t)ptr;

    /* Large allocation? */
    if (((addr - 8) & (PAGE_SIZE - 1)) == 0) {
        void *base = (void *)(addr - 8);
        uint32_t order = (uint32_t)(*(uint64_t *)base);
        /* Validate order is not corrupted before computing size */
        if (order > PMM_MAX_ORDER) {
            LOG_ERROR("kmalloc: corrupted large header in usable_size: order=%u", order);
            return 0;
        }
        return ((1UL << order) * PAGE_SIZE) - 8;
    }

    /* Slab allocation — size is the class obj_size */
    struct slab_header *slab = slab_from_ptr(ptr);
    return slab->obj_size;
}

/* ── krealloc ─────────────────────────────────────────────────── */

void *krealloc(void *ptr, uint64_t new_size) {
    if (ptr == NULL) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    uint64_t old_size = kmalloc_usable_size(ptr);

    /* If new_size fits in the same size class, return as-is */
    if (new_size <= old_size) {
        int old_cls = size_to_class(old_size);
        int new_cls = size_to_class(new_size);
        if (old_cls == new_cls && old_cls >= 0) return ptr;
    }

    /* Allocate new, copy, free old */
    void *new_ptr = kmalloc(new_size);
    if (new_ptr == NULL) return NULL;

    uint64_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    kfree(ptr);
    return new_ptr;
}

/* ── Diagnostics ──────────────────────────────────────────────── */

void kmalloc_dump_stats(void) {
    LOG_INFO("Kernel Heap Stats:");
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (classes[i].total_allocs > 0) {
            uint32_t in_use = (uint32_t)(classes[i].total_allocs - classes[i].total_frees);

            /* Count slabs */
            uint32_t slab_count = 0;
            struct slab_header *s = classes[i].slabs;
            while (s) { slab_count++; s = s->next; }

            LOG_INFO("  [%4u B] %u active, %lu allocs, %lu frees, %u slabs",
                     classes[i].obj_size, in_use,
                     classes[i].total_allocs, classes[i].total_frees,
                     slab_count);
        }
    }
    if (large_allocs > 0) {
        LOG_INFO("  [Large ] %lu allocs, %lu frees", large_allocs, large_frees);
    }
}
