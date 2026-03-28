/*
 * Anykernel OS — Mutex with Priority Inheritance
 *
 * Implements the "priority inheritance protocol" (PIP):
 * When thread H (high priority) blocks on a mutex held by thread L
 * (low priority), L's priority is temporarily boosted to H's priority.
 * This ensures L runs quickly, releases the mutex, and H can proceed.
 *
 * Without PIP: H waits while L gets preempted by medium-priority tasks.
 * With PIP:    L runs at H's priority, finishes fast, H proceeds.
 *
 * This is the same mechanism used by macOS/XNU (turnstiles) and
 * Linux (rt_mutex). It's what saved the Mars Pathfinder.
 */

#include "mutex.h"
#include "task.h"
#include "sched.h"
#include "spinlock.h"
#include "compiler.h"
#include "kprintf.h"
#include "log.h"

/* ── mutex_lock with priority inheritance ────────────────────── */

void mutex_lock(struct mutex *m) {
    while (1) {
        uint64_t irq_flags;
        spin_lock_irqsave(&m->waiters.lock, &irq_flags);

        if (likely(!m->locked)) {
            /* Acquired! Record ownership */
            m->locked = true;
            m->owner = task_current();
            m->owner_tid = task_current()->tid;
            m->saved_priority = task_current()->priority;
            m->priority_boosted = false;
            spin_unlock_irqrestore(&m->waiters.lock, irq_flags);
            return;
        }

        /* Already locked — check if we should boost the owner */
        struct task *cur = task_current();
        struct task *owner = m->owner;

        if (owner && cur->priority < owner->priority) {
            /* Priority inheritance: boost owner to our priority
             * (lower number = higher priority) */
            if (!m->priority_boosted) {
                m->saved_priority = owner->priority;
                m->priority_boosted = true;
            }
            owner->priority = cur->priority;
        }

        /* Sleep on wait queue */
        cur->state = TASK_SLEEPING;
        list_push_back(&m->waiters.waiters, &cur->run_node);

        spin_unlock_irqrestore(&m->waiters.lock, irq_flags);
        schedule();
        /* When we wake, loop back to try again */
    }
}

/* ── mutex_unlock with priority restoration ─────────────────── */

void mutex_unlock(struct mutex *m) {
    uint64_t irq_flags;
    spin_lock_irqsave(&m->waiters.lock, &irq_flags);

    /* Restore owner's original priority if it was boosted */
    if (m->priority_boosted && m->owner) {
        m->owner->priority = m->saved_priority;
        m->priority_boosted = false;
    }

    m->locked = false;
    m->owner = 0;
    m->owner_tid = 0;

    /* Wake one waiter if any */
    if (!list_empty(&m->waiters.waiters)) {
        struct list_node *node = m->waiters.waiters.sentinel.next;
        list_remove_node(node);
        struct task *t = container_of(node, struct task, run_node);
        t->state = TASK_READY;

        extern void sched_add_ready(struct task *t);
        sched_add_ready(t);
    }

    spin_unlock_irqrestore(&m->waiters.lock, irq_flags);
}

/* ── mutex_trylock ───────────────────────────────────────────── */

bool mutex_trylock(struct mutex *m) {
    uint64_t irq_flags;
    spin_lock_irqsave(&m->waiters.lock, &irq_flags);

    if (likely(!m->locked)) {
        m->locked = true;
        m->owner = task_current();
        m->owner_tid = task_current()->tid;
        m->saved_priority = task_current()->priority;
        m->priority_boosted = false;
        spin_unlock_irqrestore(&m->waiters.lock, irq_flags);
        return true;
    }

    spin_unlock_irqrestore(&m->waiters.lock, irq_flags);
    return false;
}
