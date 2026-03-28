/*
 * Anykernel OS — Wait Queue Implementation
 */

#include "waitqueue.h"
#include "sched.h"
#include "task.h"
#include "list.h"
#include "spinlock.h"

/* ── wq_wait ─────────────────────────────────────────────────── */

void wq_wait(struct wait_queue *wq) {
    uint64_t irq_flags;
    spin_lock_irqsave(&wq->lock, &irq_flags);

    /* Add current task to wait queue */
    struct task *cur = task_current();
    cur->state = TASK_SLEEPING;
    list_push_back(&wq->waiters, &cur->run_node);

    spin_unlock_irqrestore(&wq->lock, irq_flags);

    /* Yield CPU — scheduler won't pick us since state is SLEEPING */
    schedule();
}

/* ── wq_wake_one ─────────────────────────────────────────────── */

bool wq_wake_one(struct wait_queue *wq) {
    uint64_t irq_flags;
    spin_lock_irqsave(&wq->lock, &irq_flags);

    if (list_empty(&wq->waiters)) {
        spin_unlock_irqrestore(&wq->lock, irq_flags);
        return false;
    }

    struct list_node *node = wq->waiters.sentinel.next;
    list_remove_node(node);
    struct task *t = container_of(node, struct task, run_node);
    t->state = TASK_READY;

    /* Re-add to scheduler ready queue */
    extern void sched_add_ready(struct task *t);
    sched_add_ready(t);

    spin_unlock_irqrestore(&wq->lock, irq_flags);
    return true;
}

/* ── wq_wake_all ─────────────────────────────────────────────── */

int wq_wake_all(struct wait_queue *wq) {
    uint64_t irq_flags;
    spin_lock_irqsave(&wq->lock, &irq_flags);

    int count = 0;
    extern void sched_add_ready(struct task *t);

    while (!list_empty(&wq->waiters)) {
        struct list_node *node = wq->waiters.sentinel.next;
        list_remove_node(node);
        struct task *t = container_of(node, struct task, run_node);
        t->state = TASK_READY;
        sched_add_ready(t);
        count++;
    }

    spin_unlock_irqrestore(&wq->lock, irq_flags);
    return count;
}
