/*
 * Anykernel OS — Wait Queue
 *
 * Sleeping wait mechanism. Threads block on a wait queue until
 * another thread (or IRQ handler) wakes them.
 *
 * This replaces busy-waiting (polling) with efficient CPU-yielding:
 *   - wq_wait()   — sleep until woken
 *   - wq_wake_one — wake the first waiter
 *   - wq_wake_all — wake all waiters
 *
 * Used by: mutex, semaphore, task_join, future IPC
 */

#ifndef WAITQUEUE_H
#define WAITQUEUE_H

#include "list.h"
#include "spinlock.h"
#include <stdbool.h>

struct wait_queue {
    struct list_head waiters;    /* list of waiting tasks */
    spinlock_t       lock;
};

#define WAIT_QUEUE_INIT(name) {             \
    .waiters = LIST_HEAD_INIT((name).waiters), \
    .lock    = SPINLOCK_INIT                \
}

/* Initialize a wait queue at runtime */
static inline void wq_init(struct wait_queue *wq) {
    list_head_init(&wq->waiters);
    wq->lock = (spinlock_t)SPINLOCK_INIT;
}

/*
 * Sleep until woken. The caller MUST check its condition after
 * waking — spurious wakeups are possible.
 *
 * Usage:
 *   while (!condition) {
 *       wq_wait(&my_wq);
 *   }
 */
void wq_wait(struct wait_queue *wq);

/* Wake the first waiting thread (FIFO). Returns true if someone was woken. */
bool wq_wake_one(struct wait_queue *wq);

/* Wake ALL waiting threads. Returns count woken. */
int wq_wake_all(struct wait_queue *wq);

#endif /* WAITQUEUE_H */
