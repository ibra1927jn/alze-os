/*
 * Anykernel OS — Bitmap Resource Allocator (Windows handle table-inspired)
 *
 * O(1) allocation and deallocation of small integer IDs using a bitmap.
 * Windows uses this for handle tables, GDI objects, thread IDs.
 * Linux uses similar for pid allocation (pidmap).
 *
 * Usage:
 *   struct id_pool pool;
 *   id_pool_init(&pool);
 *   int id = id_alloc(&pool);     // O(1) via BSF
 *   id_free(&pool, id);           // O(1) bit clear
 */

#ifndef BITMAP_ALLOC_H
#define BITMAP_ALLOC_H

#include <stdint.h>
#include "spinlock.h"

#define ID_POOL_BITS  64  /* 64 IDs per pool (1 uint64_t) */

struct id_pool {
    volatile uint64_t bitmap;  /* 1 = free, 0 = used */
    spinlock_t        lock;
};

#define ID_POOL_INIT { .bitmap = ~0ULL, .lock = SPINLOCK_INIT }

static inline void id_pool_init(struct id_pool *pool) {
    pool->bitmap = ~0ULL;  /* All bits free */
    pool->lock = (spinlock_t)SPINLOCK_INIT;
}

/* Allocate an ID. Returns 0-63 on success, -1 if full. */
static inline int id_alloc(struct id_pool *pool) {
    uint64_t flags;
    spin_lock_irqsave(&pool->lock, &flags);

    if (pool->bitmap == 0) {
        spin_unlock_irqrestore(&pool->lock, flags);
        return -1;  /* Full */
    }

    /* BSF: find first set bit = first free ID (O(1) on x86) */
    int id;
    asm volatile("bsfq %1, %q0" : "=r"(id) : "rm"(pool->bitmap));
    pool->bitmap &= ~(1ULL << id);  /* Mark used */

    spin_unlock_irqrestore(&pool->lock, flags);
    return id;
}

/* Free an ID back to the pool */
static inline void id_free(struct id_pool *pool, int id) {
    if (id < 0 || id >= ID_POOL_BITS) return;
    uint64_t flags;
    spin_lock_irqsave(&pool->lock, &flags);
    pool->bitmap |= (1ULL << id);  /* Mark free */
    spin_unlock_irqrestore(&pool->lock, flags);
}

/* Check if an ID is in use */
static inline int id_is_used(struct id_pool *pool, int id) {
    if (id < 0 || id >= ID_POOL_BITS) return 0;
    return !(pool->bitmap & (1ULL << id));
}

static inline int id_pool_count_free(struct id_pool *pool) {
    return __builtin_popcountll(pool->bitmap);
}

#endif /* BITMAP_ALLOC_H */
