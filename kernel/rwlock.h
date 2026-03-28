/*
 * Anykernel OS — Read-Write Lock (rwlock)
 *
 * Allows multiple concurrent readers OR one exclusive writer.
 * Essential for data structures that are read-heavy and written rarely
 * (e.g., the VFS device table, VMA region lists, routing tables).
 *
 * Implementation: ticket-based with reader count.
 * - Readers: increment reader count atomically
 * - Writers: acquire exclusive access via spinlock + wait for readers to drain
 *
 * Priority: writers take priority to prevent writer starvation.
 *
 * Inspired by: Linux rwlock_t, macOS os_unfair_lock (reader-writer variant).
 */

#ifndef RWLOCK_H
#define RWLOCK_H

#include <stdint.h>
#include "spinlock.h"

struct rwlock {
    spinlock_t       lock;       /* Protects state transitions */
    volatile int32_t readers;    /* Number of active readers */
    volatile int32_t writer;     /* 1 if a writer is active */
    volatile int32_t w_waiting;  /* Number of writers waiting */
};

#define RWLOCK_INIT { .lock = SPINLOCK_INIT, .readers = 0, .writer = 0, .w_waiting = 0 }

static inline void rwlock_init(struct rwlock *rw) {
    rw->lock = (spinlock_t)SPINLOCK_INIT;
    rw->readers = 0;
    rw->writer = 0;
    rw->w_waiting = 0;
}

/* ── Read lock (shared) ──────────────────────────────────────── */

static inline void rwlock_read_lock(struct rwlock *rw) {
    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&rw->lock, &flags);

        /* Writers take priority: don't enter if a writer is waiting */
        if (!rw->writer && rw->w_waiting == 0) {
            rw->readers++;
            spin_unlock_irqrestore(&rw->lock, flags);
            return;
        }

        spin_unlock_irqrestore(&rw->lock, flags);
        asm volatile("pause" ::: "memory");
    }
}

static inline void rwlock_read_unlock(struct rwlock *rw) {
    uint64_t flags;
    spin_lock_irqsave(&rw->lock, &flags);
    rw->readers--;
    spin_unlock_irqrestore(&rw->lock, flags);
}

/* ── Write lock (exclusive) ──────────────────────────────────── */

static inline void rwlock_write_lock(struct rwlock *rw) {
    uint64_t flags;
    spin_lock_irqsave(&rw->lock, &flags);
    rw->w_waiting++;
    spin_unlock_irqrestore(&rw->lock, flags);

    for (;;) {
        spin_lock_irqsave(&rw->lock, &flags);
        if (rw->readers == 0 && !rw->writer) {
            rw->writer = 1;
            rw->w_waiting--;
            spin_unlock_irqrestore(&rw->lock, flags);
            return;
        }
        spin_unlock_irqrestore(&rw->lock, flags);
        asm volatile("pause" ::: "memory");
    }
}

static inline void rwlock_write_unlock(struct rwlock *rw) {
    uint64_t flags;
    spin_lock_irqsave(&rw->lock, &flags);
    rw->writer = 0;
    spin_unlock_irqrestore(&rw->lock, flags);
}

#endif /* RWLOCK_H */
