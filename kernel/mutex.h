/*
 * Anykernel OS — Mutex with Priority Inheritance (XNU-inspired)
 *
 * When a high-priority thread blocks on a mutex held by a low-priority
 * thread, the holder's priority is temporarily boosted to the waiter's
 * priority. This prevents "priority inversion" — a classic real-time
 * bug that crashed the Mars Pathfinder in 1997.
 *
 * macOS/XNU uses "turnstiles" for this. Our implementation is simpler
 * but achieves the same correctness guarantee.
 *
 * WARNING: Mutexes MUST NOT be used in interrupt context.
 *          Use spinlocks with IRQ save for that.
 */

#ifndef MUTEX_H
#define MUTEX_H

#include "waitqueue.h"
#include "task.h"
#include <stdbool.h>
#include <stdint.h>

struct mutex {
    volatile bool     locked;
    struct wait_queue  waiters;
    uint32_t           owner_tid;       /* TID of holding thread */
    struct task       *owner;           /* Pointer to owner task (for PI) */
    enum task_priority saved_priority;  /* Owner's original priority (restored on unlock) */
    bool               priority_boosted; /* Was owner's priority boosted? */
};

#define MUTEX_INIT(name) {              \
    .locked  = false,                   \
    .waiters = WAIT_QUEUE_INIT((name).waiters), \
    .owner_tid = 0,                     \
    .owner = 0,                         \
    .saved_priority = 32,               \
    .priority_boosted = false           \
}

/* Initialize a mutex at runtime */
static inline void mutex_init(struct mutex *m) {
    m->locked = false;
    wq_init(&m->waiters);
    m->owner_tid = 0;
    m->owner = 0;
    m->saved_priority = 32; /* TASK_PRIO_NORMAL */
    m->priority_boosted = false;
}

/*
 * Acquire the mutex. Sleeps if already held.
 * If the holder has lower priority, boosts it (priority inheritance).
 * MUST NOT be called from interrupt context.
 */
void mutex_lock(struct mutex *m);

/*
 * Release the mutex. Restores priority if boosted. Wakes one waiter.
 */
void mutex_unlock(struct mutex *m);

/*
 * Try to acquire without sleeping. Returns true if acquired.
 */
bool mutex_trylock(struct mutex *m);

#endif /* MUTEX_H */
