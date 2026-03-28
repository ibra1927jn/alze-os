/*
 * Anykernel OS — Counting Semaphore Implementation
 */

#include "semaphore.h"
#include "task.h"
#include "sched.h"
#include "list.h"
#include "errno.h"

/* ── sem_wait — decrement or sleep ───────────────────────────── */

void sem_wait(struct semaphore *sem) {
    while (1) {
        uint64_t irq_flags;
        spin_lock_irqsave(&sem->lock, &irq_flags);

        if (sem->count > 0) {
            sem->count--;
            spin_unlock_irqrestore(&sem->lock, irq_flags);
            return;
        }

        /* Count is 0 — sleep on wait queue */
        struct task *cur = task_current();
        cur->state = TASK_SLEEPING;
        list_push_back(&sem->waiters.waiters, &cur->run_node);

        spin_unlock_irqrestore(&sem->lock, irq_flags);
        schedule();
        /* Loop back to check again after wake */
    }
}

/* ── sem_post — increment and wake ───────────────────────────── */

void sem_post(struct semaphore *sem) {
    uint64_t irq_flags;
    spin_lock_irqsave(&sem->lock, &irq_flags);

    sem->count++;

    /* Wake one waiter if any */
    if (!list_empty(&sem->waiters.waiters)) {
        struct list_node *node = sem->waiters.waiters.sentinel.next;
        list_remove_node(node);
        struct task *t = container_of(node, struct task, run_node);
        t->state = TASK_READY;
        sched_add_ready(t);
    }

    spin_unlock_irqrestore(&sem->lock, irq_flags);
}

/* ── sem_trywait — non-blocking ──────────────────────────────── */

int sem_trywait(struct semaphore *sem) {
    uint64_t irq_flags;
    spin_lock_irqsave(&sem->lock, &irq_flags);

    if (sem->count > 0) {
        sem->count--;
        spin_unlock_irqrestore(&sem->lock, irq_flags);
        return 0;
    }

    spin_unlock_irqrestore(&sem->lock, irq_flags);
    return -EAGAIN;
}
