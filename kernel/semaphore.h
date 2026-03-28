/*
 * Anykernel OS — Counting Semaphore
 *
 * A semaphore manages a counter of available resources.
 * sem_wait() decrements (blocks if 0), sem_post() increments.
 *
 * Use cases:
 *   - Producer/consumer (bounded buffer)
 *   - Limiting concurrent access to N resources
 *   - Binary semaphore (init count=1) = mutex alternative
 */

#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "waitqueue.h"
#include <stdint.h>

struct semaphore {
    volatile int      count;
    struct wait_queue  waiters;
    spinlock_t         lock;
};

#define SEMAPHORE_INIT(name, n) {              \
    .count   = (n),                            \
    .waiters = WAIT_QUEUE_INIT((name).waiters),\
    .lock    = SPINLOCK_INIT                   \
}

/* Initialize at runtime with given count */
static inline void sem_init(struct semaphore *sem, int count) {
    sem->count = count;
    wq_init(&sem->waiters);
    sem->lock = (spinlock_t)SPINLOCK_INIT;
}

/* Decrement. Blocks if count is 0. */
void sem_wait(struct semaphore *sem);

/* Increment. Wakes one waiter if any. */
void sem_post(struct semaphore *sem);

/* Try to decrement without blocking. Returns 0 if acquired, -1 if not. */
int sem_trywait(struct semaphore *sem);

/* Get current count (snapshot, may change immediately). */
static inline int sem_getvalue(struct semaphore *sem) {
    return sem->count;
}

#endif /* SEMAPHORE_H */
