/*
 * Anykernel OS — Adaptive Mutex (macOS-inspired)
 *
 * Like macOS lck_mtx_t: if the lock holder is running on another CPU,
 * spin briefly instead of immediately sleeping. On single-core, this
 * degrades gracefully to a regular sleeping mutex.
 *
 * Combines the best of spinlocks (low latency) and mutexes (no CPU waste):
 *   - First: spin for ADAPTIVE_SPIN_LIMIT iterations
 *   - Then: sleep like a normal mutex (with priority inheritance)
 *
 * Usage:
 *   struct adaptive_mutex amtx = ADAPTIVE_MUTEX_INIT;
 *   adaptive_lock(&amtx);
 *   // critical section
 *   adaptive_unlock(&amtx);
 */

#ifndef ADAPTIVE_MUTEX_H
#define ADAPTIVE_MUTEX_H

#include "task.h"
#include "spinlock.h"
#include "waitqueue.h"
#include "sched.h"
#include "compiler.h"
#include <stdint.h>

#define ADAPTIVE_SPIN_LIMIT  100  /* Iterations before sleeping */

struct adaptive_mutex {
    volatile int         locked;
    struct task          *owner;
    spinlock_t           guard;      /* Protects wait queue */
    struct wait_queue    waiters;
    uint8_t              owner_prio; /* For priority inheritance */
};

#define ADAPTIVE_MUTEX_INIT { .locked = 0, .owner = 0, \
    .guard = SPINLOCK_INIT, .waiters = WAIT_QUEUE_INIT, .owner_prio = 0 }

static inline void adaptive_lock(struct adaptive_mutex *m) {
    /* Phase 1: Optimistic spin (macOS-style adaptive) */
    for (int i = 0; i < ADAPTIVE_SPIN_LIMIT; i++) {
        if (!__sync_lock_test_and_set(&m->locked, 1)) {
            m->owner = task_current();
            return;  /* Acquired via spin — fast path */
        }
        asm volatile("pause");  /* Reduce pipeline contention */
    }

    /* Phase 2: Sleep (mutex-style with PI) */
    uint64_t flags;
    spin_lock_irqsave(&m->guard, &flags);

    while (__sync_lock_test_and_set(&m->locked, 1)) {
        /* Priority inheritance: boost owner if needed */
        struct task *cur = task_current();
        if (m->owner && cur->priority < m->owner->priority) {
            m->owner_prio = m->owner->priority;
            m->owner->priority = cur->priority;
        }
        spin_unlock_irqrestore(&m->guard, flags);
        wq_wait(&m->waiters);
        spin_lock_irqsave(&m->guard, &flags);
    }

    m->owner = task_current();
    spin_unlock_irqrestore(&m->guard, flags);
}

static inline void adaptive_unlock(struct adaptive_mutex *m) {
    uint64_t flags;
    spin_lock_irqsave(&m->guard, &flags);

    /* Restore priority if we boosted it */
    if (m->owner_prio > 0) {
        m->owner->priority = m->owner_prio;
        m->owner_prio = 0;
    }

    m->owner = 0;
    __sync_lock_release(&m->locked);
    wq_wake_one(&m->waiters);

    spin_unlock_irqrestore(&m->guard, flags);
}

#endif /* ADAPTIVE_MUTEX_H */
