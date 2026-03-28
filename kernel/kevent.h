/*
 * Anykernel OS — Event Objects (Windows KEVENT-inspired)
 *
 * Lightweight signaling primitive for thread synchronization.
 * Two modes:
 *   - Manual reset: stays signaled until explicitly reset
 *   - Auto reset:   wakes one waiter and auto-clears
 *
 * Windows uses KEVENT extensively for driver/kernel coordination.
 * Simpler than semaphores for pure signal/wait patterns.
 *
 * Usage:
 *   struct kevent evt;
 *   kevent_init(&evt, KEVENT_AUTO_RESET);
 *   // Thread A: kevent_wait(&evt);     // blocks
 *   // Thread B: kevent_signal(&evt);   // wakes A
 */

#ifndef KEVENT_H
#define KEVENT_H

#include "spinlock.h"
#include "waitqueue.h"
#include "sched.h"

enum kevent_type {
    KEVENT_MANUAL_RESET = 0,  /* Stays signaled until reset */
    KEVENT_AUTO_RESET   = 1,  /* Auto-clears after waking one */
};

struct kevent {
    volatile int         signaled;
    enum kevent_type     type;
    spinlock_t           lock;
    struct wait_queue    waiters;
};

#define KEVENT_INIT(t) { .signaled = 0, .type = (t), \
    .lock = SPINLOCK_INIT, .waiters = WAIT_QUEUE_INIT }

static inline void kevent_init(struct kevent *e, enum kevent_type type) {
    e->signaled = 0;
    e->type = type;
    e->lock = (spinlock_t)SPINLOCK_INIT;
    wq_init(&e->waiters);
}

static inline void kevent_signal(struct kevent *e) {
    uint64_t flags;
    spin_lock_irqsave(&e->lock, &flags);
    e->signaled = 1;
    if (e->type == KEVENT_AUTO_RESET)
        wq_wake_one(&e->waiters);
    else
        wq_wake_all(&e->waiters);
    spin_unlock_irqrestore(&e->lock, flags);
}

static inline void kevent_wait(struct kevent *e) {
    uint64_t flags;
    spin_lock_irqsave(&e->lock, &flags);
    while (!e->signaled) {
        spin_unlock_irqrestore(&e->lock, flags);
        wq_wait(&e->waiters);
        spin_lock_irqsave(&e->lock, &flags);
    }
    if (e->type == KEVENT_AUTO_RESET)
        e->signaled = 0;  /* Auto-clear after waking */
    spin_unlock_irqrestore(&e->lock, flags);
}

static inline void kevent_reset(struct kevent *e) {
    e->signaled = 0;
}

static inline int kevent_is_signaled(struct kevent *e) {
    return e->signaled;
}

#endif /* KEVENT_H */
